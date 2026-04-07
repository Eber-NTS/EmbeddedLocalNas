#include "DatabaseManager.h"
#include "SD_MMC.h"

//database object variable from SQLite library
sqlite3 *db = nullptr;


bool initDatabase() {
    //sets up SQLite library, allocating global data structures and mem allocators to function properly
    //initializes the library to hardware specifications and the default virtual file system for the esp32 platform
    sqlite3_initialize();
    //using the /sdcard path created in storageManager.cpp to mount microSD to VFS, we use the /sdcard/index.db
    //to describe where we want our database pointer mounted to. We then check if this path opens correctly.
    if (sqlite3_open("/sdcard/index.db", &db) == SQLITE_OK) {
        //creates a new table object, and deletes already existing tables.
        sqlite3_exec(db, "DROP TABLE IF EXISTS FILES;", NULL, NULL, NULL);
        sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS FILES (NAME TEXT, PARENT_DIR TEXT, IS_FOLDER INT, SIZE INT);", NULL, NULL, NULL);
        return true;
    }
    return false;
}



void indexInternalDrive(String targetDir) {
    //tells sqlite to pause writing every single change directly to the physical micro sd.
    //Opens a batch style input mode, that queues all consecutive db commands into esp32's ram.
    sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
    //deletes all files from the files table
    sqlite3_exec(db, "DELETE FROM FILES;", NULL, NULL, NULL);

    //check if file at targetDir exists and is a directory
    File root = SD_MMC.open(targetDir);
    if (!root || !root.isDirectory()) {
        //COMMIT closes the empty transaction bash mode to prevent the db from being locked in batch mode.
        sqlite3_exec(db, "COMMIT;",NULL, NULL, NULL);
        return;
    }

    File file = root.openNextFile();
    while (file) {
        String fileName = file.name();
        //if file name starts with /, we remove it to have a clean name
        if (fileName.startsWith("/")) fileName = fileName.substring(1);
        
        String lowerName = fileName;
        lowerName.toLowerCase();
        
        //conditions makes sure system files are hidden from the browser (case-insensitive)
        if (lowerName != "index.db" && lowerName != "index.db-journal" && lowerName != "index.html" && lowerName != "login.html") {
            //collect data and insert into database
            int isDir = file.isDirectory() ? 1 : 0;
            int size = file.size();
            char *zSQL = sqlite3_mprintf("INSERT INTO FILES (NAME, PARENT_DIR, IS_FOLDER, SIZE) VALUES ('%q', '%q', %d, %d);", fileName.c_str(), targetDir.c_str(), isDir, size);
            sqlite3_exec(db, zSQL, NULL, NULL, NULL);
            //prevents memory leek from dynamic mem allocation resulting from query
            sqlite3_free(zSQL);
        }
        //rewinds
        file = root.openNextFile();
    }
    //closes batch mode
    sqlite3_exec(db, "COMMIT;",NULL, NULL, NULL);
}
