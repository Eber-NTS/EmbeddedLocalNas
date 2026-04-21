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
        // Use memory for temp files and journaling to prevent SD card IO locking errors
        sqlite3_exec(db, "PRAGMA journal_mode = MEMORY;", NULL, NULL, NULL);
        sqlite3_exec(db, "PRAGMA temp_store = MEMORY;", NULL, NULL, NULL);

        //creates a new table object, and deletes already existing tables.
        sqlite3_exec(db, "DROP TABLE IF EXISTS FILES;", NULL, NULL, NULL);
        sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS FILES (NAME TEXT, PARENT_DIR TEXT, IS_FOLDER INT, SIZE INT, LAST_MODIFIED INT);", NULL, NULL, NULL);
        sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS USERS (USERNAME TEXT PRIMARY KEY, PASSWORD TEXT, ROLE INT DEFAULT 0);", NULL, NULL, NULL);
        
        // Ensures older databases are updated with the new ROLE column if it was missing previously.
        // This will safely fail (and do nothing) if the column already exists.
        sqlite3_exec(db, "ALTER TABLE USERS ADD COLUMN ROLE INT DEFAULT 0;", NULL, NULL, NULL);

        // Self-Healing Check: Verify the USERS table actually has the ROLE column.
        // If ALTER TABLE failed (common on embedded SQLite), queries will permanently fail.
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, "SELECT ROLE FROM USERS LIMIT 1;", -1, &stmt, NULL) != SQLITE_OK) {
            Serial.println("Schema mismatch detected! Rebuilding USERS table...");
            sqlite3_exec(db, "DROP TABLE IF EXISTS USERS;", NULL, NULL, NULL);
            sqlite3_exec(db, "CREATE TABLE USERS (USERNAME TEXT PRIMARY KEY, PASSWORD TEXT, ROLE INT DEFAULT 0);", NULL, NULL, NULL);
        } else {
            sqlite3_finalize(stmt);
        }

        // Seed the database with a default Admin account.
        // The "OR IGNORE" ensures it doesn't overwrite the password if you change it later!
        sqlite3_exec(db, "INSERT OR IGNORE INTO USERS (USERNAME, PASSWORD, ROLE) VALUES ('admin', 'admin', 1);", NULL, NULL, NULL);
        
        return true;
    }
    return false;
}



void indexInternalDrive(String targetDir) {
    //tells sqlite to pause writing every single change directly to the physical micro sd.
    //Opens a batch style input mode, that queues all consecutive db commands into esp32's ram.
    sqlite3_exec(db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
    
    // Deletes only the records for the specific directory being re-indexed.
    char* deleteQuery = sqlite3_mprintf("DELETE FROM FILES WHERE PARENT_DIR = '%q';", targetDir.c_str());
    sqlite3_exec(db, deleteQuery, NULL, NULL, NULL);
    sqlite3_free(deleteQuery);

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
            time_t lastMod = file.getLastWrite();
            char *zSQL = sqlite3_mprintf("INSERT INTO FILES (NAME, PARENT_DIR, IS_FOLDER, SIZE, LAST_MODIFIED) VALUES ('%q', '%q', %d, %d, %lld);", fileName.c_str(), targetDir.c_str(), isDir, size, (long long)lastMod);
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

//Takes a userName and password sent from user to create an account to be used for login. The data is inserted into the User's table
bool createUser(String username, String password, int role) {
    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO USERS (USERNAME, PASSWORD, ROLE) VALUES (?, ?, ?);";
    
    // Prepare the statement safely
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, password.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, role);

        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        
        // Log error to the serial monitor for debugging if the insert fails
        if (rc != SQLITE_DONE) {
            Serial.printf("SQLite Insert Error: %s\n", sqlite3_errmsg(db));
        }
        
        return rc == SQLITE_DONE; // Returns true if insertion succeeded
    } else {
        Serial.printf("SQLite Prepare Error: %s\n", sqlite3_errmsg(db));
    }
    return false;
}

// Returns the role of the user if valid, otherwise -1
int verifyUser(String username, String password) {
    sqlite3_stmt *stmt;
    int role = -1;
    const char *sql = "SELECT PASSWORD, ROLE FROM USERS WHERE USERNAME = ?;";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            String dbPass = (const char*)sqlite3_column_text(stmt, 0);
            if (dbPass == password) {
                role = sqlite3_column_int(stmt, 1);
            }
        }
        sqlite3_finalize(stmt);
    }
    return role;
}
