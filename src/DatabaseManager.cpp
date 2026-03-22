#include "DatabaseManager.h"
#include "SD_MMC.h"

sqlite3 *db = nullptr;

bool initDatabase() {
    sqlite3_initialize();
    if (sqlite3_open("/sdcard/index.db", &db) == SQLITE_OK) {
        sqlite3_exec(db, "DROP TABLE IF EXISTS FILES;", NULL, NULL, NULL);
        sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS FILES (NAME TEXT, PARENT_DIR TEXT, IS_FOLDER INT, SIZE INT);", NULL, NULL, NULL);
        return true;
    }
    return false;
}

void indexInternalDrive(String targetDir) {
    sqlite3_exec(db, "DELETE FROM FILES;", NULL, NULL, NULL);

    File root = SD_MMC.open(targetDir);
    if (!root || !root.isDirectory()) return;

    File file = root.openNextFile();
    while (file) {
        String fileName = file.name();
        if (fileName.startsWith("/")) fileName = fileName.substring(1);

        if (fileName != "index.db") {
            int isDir = file.isDirectory() ? 1 : 0;
            int size = file.size();

            char *zSQL = sqlite3_mprintf("INSERT INTO FILES (NAME, PARENT_DIR, IS_FOLDER, SIZE) VALUES ('%q', '%q', %d, %d);", fileName.c_str(), targetDir.c_str(), isDir, size);
            sqlite3_exec(db, zSQL, NULL, NULL, NULL);
            sqlite3_free(zSQL);
        }
        file = root.openNextFile();
    }
}
