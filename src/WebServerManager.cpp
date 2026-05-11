#include "WebServerManager.h"
#include "StorageManager.h"
#include "DatabaseManager.h"
#include "SD_MMC.h"
#include <vector>
#include <sys/time.h>
#include <WiFi.h>

WebServer server(80);
File fsUploadFile;

// Struct to hold session data
struct Session {
    String token;
    String username;
    int role;
};

// Store multiple active session objects
std::vector<Session> activeSessions;

// Helper to get the current session based on the incoming cookie
Session* getCurrentSession() {
    if (server.hasHeader("Cookie")) {
        String cookie = server.header("Cookie");

        for (auto& session : activeSessions) {
            int startIndex = cookie.indexOf(session.token);
            if (startIndex != -1) {
                // Ensure the match is exact and not just a prefix of another value
                int endIndex = startIndex + session.token.length();
                if (endIndex == cookie.length() || cookie[endIndex] == ';') {
                    return &session;
                }
            }
        }
    }
    return nullptr;
}

// verifies if incoming http request is coming from a user who had already logged in before
bool isAuthenticated() {
    return getCurrentSession() != nullptr;
}

// checks if current session is an admin
bool requireAdmin() {
    Session* sess = getCurrentSession();
    return sess != nullptr && sess->role >= 2;
}

// checks if current session has write/upload permissions (Standard User or higher)
bool requireWriteAccess() {
    Session* sess = getCurrentSession();
    return sess != nullptr && sess->role >= 1;
}

// Helper to securely sync time from an incoming request payload
void trySyncTime() {
    if (server.hasArg("time")) {
        long timestamp = server.arg("time").toInt();
        // Sanity check to ensure valid modern timestamp (e.g. > Year 2020)
        if (timestamp > 1600000000) {
            struct timeval tv;
            tv.tv_sec = timestamp;
            tv.tv_usec = 0;
            settimeofday(&tv, NULL);
        }
    }
}

//Processes the form submission from a user login attempt.
void handleLogin() {
    trySyncTime(); // Sync time if provided in the login form
    if (server.hasArg("username") && server.hasArg("password")) {
        String username = server.arg("username");
        String password = server.arg("password");

        username.trim();
        password.trim();

        int role = verifyUser(username, password);
        if (role != -1) {
            // Generate a random session token upon successful login
            logActivity(username, "LOGIN", "Successful login");
            String token = "ESP32_SESSION=" + String(esp_random());
            activeSessions.push_back({token, username, role});

            server.sendHeader("Set-Cookie", token + "; Path=/; HttpOnly");
            server.sendHeader("Location", "/");
            server.send(303);
            return;
        }
    }
    // Redirect back to login with error
    server.sendHeader("Location", "/login.html?error=1");
    server.send(303);
}

// Processes new account creation
void handleRegister() {
    trySyncTime(); // Sync time if provided in the register form
    if (server.hasArg("username") && server.hasArg("password")) {
        String username = server.arg("username");
        String password = server.arg("password");

        username.trim();
        password.trim();

        // Prevent creating users with empty credentials
        if (username.length() == 0 || password.length() == 0) {
            server.sendHeader("Location", "/login.html?error=3");
            server.send(303);
            return;
        }

        // Attempt to create user (will fail if username exists due to PRIMARY KEY)
        // Default role is 1 (standard user)
        if (createUser(username, password, 1)) {
            // Success, send back to login with success message
            server.sendHeader("Location", "/login.html?success=1");
            server.send(303);
            return;
        } else {
            // Registration failed (likely duplicate username)
            server.sendHeader("Location", "/login.html?error=2");
            server.send(303);
            return;
        }
    }
    server.sendHeader("Location", "/login.html?error=3");
    server.send(303);
}

// Safely destroys the session on the backend and logs the user out
void handleLogout() {
    Session* sess = getCurrentSession();
    if (sess) {
        logActivity(sess->username, "LOGOUT", "User logged out");
        
        // Find and erase the session from memory
        for (auto it = activeSessions.begin(); it != activeSessions.end(); ) {
            if (it->token == sess->token) it = activeSessions.erase(it);
            else ++it;
        }
    }
    // Force browser to clear the cookie
    server.sendHeader("Set-Cookie", "ESP32_SESSION=; expires=Thu, 01 Jan 1970 00:00:00 UTC; Path=/;");
    server.send(200, "text/plain", "Logged out");
}

//data container used to pass state between web server routing functions and the SQLite database callback function
struct RenderContext {
    String currentPath;
    String generatedHTML;
};

static int build_json_callback(void *data, int argc, char **argv, char **azColName) {
    RenderContext* ctx = (RenderContext*)data;

    if (ctx->generatedHTML.length() > 0) ctx->generatedHTML += ",";

    String name = argv[0] ? argv[0] : "Unknown";
    String isFolder = argv[1] ? argv[1] : "0";
    String size = argv[2] ? argv[2] : "0";
    String parentDir = (argc > 3 && argv[3]) ? argv[3] : "";
    String lastMod = (argc > 4 && argv[4]) ? argv[4] : "0";

    ctx->generatedHTML += "{\"name\":\"" + name + "\",\"isFolder\":" + isFolder + ",\"size\":" + size + ",\"lastMod\":" + lastMod;
    if (parentDir.length() > 0) {
        ctx->generatedHTML += ",\"parentDir\":\"" + parentDir + "\"";
    }
    ctx->generatedHTML += "}";
    return 0;
}

void handleRoot() {
    if (!isAuthenticated()) {
        server.sendHeader("Location", "/login.html");
        server.send(303);
        return;
    }
    if (SD_MMC.exists("/index.html")) {
        File file = SD_MMC.open("/index.html", "r");
        server.streamFile(file, "text/html");
        file.close();
    } else {
        server.send(404, "text/plain", "Missing index.html on drive. Please upload it.");
    }
}

void handleApiList() {
    if (!isAuthenticated()) { server.send(401, "text/plain", "Unauthorized"); return; }

    RenderContext context;
    context.generatedHTML = "";

    context.currentPath = server.hasArg("dir") ? server.arg("dir") : "/";

    // Only re-index the physical SD card if explicitly requested (or by default).
    // Background UI polling will pass index=false to safely perform a read-only database query.
    bool shouldIndex = !(server.hasArg("index") && server.arg("index") == "false");
    if (shouldIndex) {
        indexInternalDrive(context.currentPath);
    }

    String sortOrder = server.hasArg("sort") ? server.arg("sort") : "name_asc";
    String sqlSort = "NAME ASC";
    if (sortOrder == "name_desc") sqlSort = "NAME DESC";
    else if (sortOrder == "date_desc") sqlSort = "LAST_MODIFIED DESC";
    else if (sortOrder == "date_asc") sqlSort = "LAST_MODIFIED ASC";

    // We use NULL for parent_dir so that LAST_MODIFIED is perfectly aligned at index 4 for our callback
    char *query = sqlite3_mprintf("SELECT NAME, IS_FOLDER, SIZE, NULL, LAST_MODIFIED FROM FILES WHERE PARENT_DIR='%q' ORDER BY IS_FOLDER DESC, %s;", context.currentPath.c_str(), sqlSort.c_str());
    sqlite3_exec(db, query, build_json_callback, (void*)&context, NULL);
    sqlite3_free(query);

    String json = "{\"dir\":\"" + context.currentPath + "\",\"files\":[" + context.generatedHTML + "]}";
    
    server.send(200, "application/json", json);
}

void handleApiSearch() {
    if (!isAuthenticated()) { server.send(401, "text/plain", "Unauthorized"); return; }

    RenderContext context;
    context.generatedHTML = "";

    String queryStr = server.hasArg("q") ? server.arg("q") : "";
    String sortOrder = server.hasArg("sort") ? server.arg("sort") : "name_asc";
    String sqlSort = "NAME ASC";
    if (sortOrder == "name_desc") sqlSort = "NAME DESC";
    else if (sortOrder == "date_desc") sqlSort = "LAST_MODIFIED DESC";
    else if (sortOrder == "date_asc") sqlSort = "LAST_MODIFIED ASC";

    char *query = sqlite3_mprintf("SELECT NAME, IS_FOLDER, SIZE, PARENT_DIR, LAST_MODIFIED FROM FILES WHERE NAME LIKE '%%%q%%' ORDER BY IS_FOLDER DESC, %s LIMIT 100;", queryStr.c_str(), sqlSort.c_str());
    sqlite3_exec(db, query, build_json_callback, (void*)&context, NULL);
    sqlite3_free(query);

    String json = "{\"query\":\"" + queryStr + "\",\"files\":[" + context.generatedHTML + "]}";
    server.send(200, "application/json", json);
}

void handleDownload() {
    if (!isAuthenticated()) { server.send(401, "text/plain", "Unauthorized"); return; }

    if (server.hasArg("file")) {
        String path = server.arg("file");
        if (!path.startsWith("/")) path = "/" + path;

        String lowerPath = path;
        lowerPath.toLowerCase();

        if (lowerPath == "/index.db" || lowerPath == "/index.db-journal" || lowerPath == "/index.html" || lowerPath == "/login.html" || lowerPath == "/admin.html" || lowerPath.indexOf("system volume information") != -1) {
            server.send(403, "text/plain", "Forbidden: Cannot download system files");
            return;
        }

        if (SD_MMC.exists(path)) {
            File downloadFile = SD_MMC.open(path, "r");
            server.sendHeader("Content-Disposition", "attachment; filename=\"" + path.substring(path.lastIndexOf('/') + 1) + "\"");
            server.streamFile(downloadFile, "application/octet-stream");
            downloadFile.close();
            return;
        }
    }
    server.send(404, "text/plain", "File Not Found");
}

String getContentType(String filename) {
    if (filename.endsWith(".html")) return "text/html";
    else if (filename.endsWith(".css")) return "text/css";
    else if (filename.endsWith(".js")) return "application/javascript";
    else if (filename.endsWith(".png")) return "image/png";
    else if (filename.endsWith(".jpg") || filename.endsWith(".jpeg")) return "image/jpeg";
    else if (filename.endsWith(".ico")) return "image/x-icon";
    return "text/plain";
}

void handleStaticWebFiles() {
    String path = server.uri();
    
    // Allow the login page and background assets (like favicon) to bypass the auth redirect
    bool isPublicAsset = (path == "/login.html" || path.endsWith(".ico"));
    if (!isPublicAsset && !isAuthenticated()) {
        server.sendHeader("Location", "/login.html");
        server.send(303);
        return;
    }

    String lowerPath = path;
    lowerPath.toLowerCase();

    if (lowerPath == "/admin.html" && !requireAdmin()) {
        server.sendHeader("Location", "/");
        server.send(303);
        return;
    }

    if (lowerPath.indexOf("system volume information") != -1) {
        server.send(403, "text/plain", "Forbidden: Cannot access system files");
        return;
    }

    if (SD_MMC.exists(path)) {
        File file = SD_MMC.open(path, "r");
        server.streamFile(file, getContentType(path));
        file.close();
        return;
    }
    server.send(404, "text/plain", "404: File Not Found");
}

void handleDelete() {
    if (!requireWriteAccess()) { server.send(403, "text/plain", "Forbidden: Read-Only Account"); return; }

    if (server.hasArg("file")) {
        String path = server.arg("file");
        if (!path.startsWith("/")) path = "/" + path;

        String lowerPath = path;
        lowerPath.toLowerCase();

        if (lowerPath == "/index.db" || lowerPath == "/index.db-journal" || lowerPath == "/index.html" || lowerPath == "/login.html" || lowerPath == "/admin.html" || lowerPath.indexOf("system volume information") != -1) {
            server.send(403, "text/plain", "Forbidden: Cannot delete system files");
            return;
        }

        if (deleteFileOrFolder(path)) {
            Session* sess = getCurrentSession();
            if (sess) logActivity(sess->username, "DELETE", "Deleted item: " + path);
            server.send(200, "text/plain", "Deleted successfully");
            return;
        }
    }

    server.send(500, "text/plain", "Failed to delete");
}

void handleCreateFolder() {
    if (!requireWriteAccess()) { server.send(403, "text/plain", "Forbidden: Read-Only Account"); return; }

    if (server.hasArg("dir") && server.hasArg("name")) {
        String dir = server.arg("dir");
        String name = server.arg("name");

        // Input sanitization
        name.trim();
        if (!dir.endsWith("/")) dir += "/";
        if (name.startsWith("/")) name = name.substring(1);

        // Prevent directory traversal attacks
        if (name.indexOf("..") != -1 || name.indexOf("/") != -1 || name.length() == 0) {
            server.send(400, "text/plain", "Invalid folder name");
            return;
        }

        String fullPath = dir + name;

        if (SD_MMC.exists(fullPath)) {
            server.send(409, "text/plain", "Folder already exists");
            return;
        }

        if (SD_MMC.mkdir(fullPath)) {
            Session* sess = getCurrentSession();
            if (sess) logActivity(sess->username, "CREATE_FOLDER", "Created folder: " + fullPath);
            server.send(200, "text/plain", "Folder created successfully");
        } else {
            server.send(500, "text/plain", "Failed to create folder on SD card");
        }
    } else {
        server.send(400, "text/plain", "Missing required arguments");
    }
}

void handleRename() {
    if (!requireWriteAccess()) { server.send(403, "text/plain", "Forbidden: Read-Only Account"); return; }

    if (server.hasArg("oldPath") && server.hasArg("newName")) {
        String oldPath = server.arg("oldPath");
        String newName = server.arg("newName");

        if (!oldPath.startsWith("/")) oldPath = "/" + oldPath;
        newName.trim();

        // Input sanitization
        if (newName.indexOf("..") != -1 || newName.indexOf("/") != -1 || newName.length() == 0) {
            server.send(400, "text/plain", "Invalid new name");
            return;
        }

        String lowerPath = oldPath;
        lowerPath.toLowerCase();

        if (lowerPath == "/index.db" || lowerPath == "/index.db-journal" || lowerPath == "/index.html" || lowerPath == "/login.html" || lowerPath == "/admin.html" || lowerPath.indexOf("system volume information") != -1) {
            server.send(403, "text/plain", "Forbidden: Cannot rename system files");
            return;
        }

        if (!SD_MMC.exists(oldPath)) {
            server.send(404, "text/plain", "Source file/folder not found");
            return;
        }

        int lastSlashIndex = oldPath.lastIndexOf('/');
        String parentDir = oldPath.substring(0, lastSlashIndex + 1);
        if (parentDir.length() == 0) parentDir = "/";

        String newPath = parentDir + newName;

        // Don't rename if name is the same
        if (oldPath == newPath) {
            server.send(200, "text/plain", "Name is identical");
            return;
        }

        if (SD_MMC.exists(newPath)) {
            server.send(409, "text/plain", "Destination already exists");
            return;
        }

        if (SD_MMC.rename(oldPath, newPath)) {
            // Update the database to reflect the new name (this avoids having to do a full SD index)
            // If it's a folder, we'd theoretically need to update all children's PARENT_DIR paths.
            // For simplicity and to ensure total accuracy, we will just force the next /api/list call
            // to do a full re-index. The frontend currently forces an index=true on reload.

            Session* sess = getCurrentSession();
            if (sess) logActivity(sess->username, "RENAME", "Renamed: " + oldPath + " to " + newName);

            server.send(200, "text/plain", "Renamed successfully");
        } else {
            server.send(500, "text/plain", "Failed to rename on SD card");
        }
    } else {
        server.send(400, "text/plain", "Missing required arguments");
    }
}

void handleUpload() {
    if (!requireWriteAccess()) return;

    HTTPUpload& upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        trySyncTime(); // Sync time exactly when an upload begins
        String dir = server.hasArg("dir") ? server.arg("dir") : "/";
        if (!dir.endsWith("/")) dir += "/";
        String filename = upload.filename;
        if (filename.startsWith("/")) filename = filename.substring(1);
        String path = dir + filename;

        String lowerPath = path;
        lowerPath.toLowerCase();
        if (lowerPath.indexOf("system volume information") != -1) {
            server.send(403, "text/plain", "Forbidden: Cannot upload to system folders");
            return;
        }

        Serial.printf("Receiving File: %s\n", path.c_str());
        fsUploadFile = SD_MMC.open(path, FILE_WRITE);

    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (fsUploadFile) {
            fsUploadFile.write(upload.buf, upload.currentSize);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (fsUploadFile) {
            fsUploadFile.close();
            
            String dir = server.hasArg("dir") ? server.arg("dir") : "/";
            if (!dir.endsWith("/")) dir += "/";
            String filename = upload.filename;
            if (filename.startsWith("/")) filename = filename.substring(1);
            String path = dir + filename;

            Session* sess = getCurrentSession();
            if (sess) logActivity(sess->username, "UPLOAD", "Uploaded file: " + path);
            Serial.printf("Upload Complete: %s, Size: %u bytes\n", upload.filename.c_str(), upload.totalSize);
        }
    }
}

// Receives the current time from the user's browser and updates the ESP32 system clock
void handleTimeSync() {
    if (server.hasArg("time")) {
        long timestamp = server.arg("time").toInt();
        if (timestamp > 0) {
            struct timeval tv;
            tv.tv_sec = timestamp;
            tv.tv_usec = 0;
            settimeofday(&tv, NULL);
            server.send(200, "text/plain", "Time synced successfully");
            return;
        }
    }
    server.send(400, "text/plain", "Invalid time data");
}

// Admin specific endpoints

void handleApiMe() {
    Session* sess = getCurrentSession();
    if (sess) {
        String json = "{\"username\":\"" + sess->username + "\",\"role\":" + String(sess->role) + "}";
        server.send(200, "application/json", json);
    } else {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
    }
}

static int build_users_json_callback(void *data, int argc, char **argv, char **azColName) {
    String* json = (String*)data;
    if (json->length() > 0) *json += ",";

    String username = argv[0] ? argv[0] : "Unknown";
    String role = argv[1] ? argv[1] : "0";
    String createdAt = (argc > 2 && argv[2]) ? argv[2] : "0";

    *json += "{\"username\":\"" + username + "\",\"role\":" + role + ",\"createdAt\":" + createdAt + "}";
    return 0;
}

void handleAdminUsersGet() {
    if (!requireAdmin()) { server.send(403, "text/plain", "Forbidden"); return; }

    String jsonResult = "";
    const char* sql = "SELECT USERNAME, ROLE, CREATED_AT FROM USERS;";
    sqlite3_exec(db, sql, build_users_json_callback, (void*)&jsonResult, NULL);

    server.send(200, "application/json", "[" + jsonResult + "]");
}

void handleAdminRolesPost() {
    if (!requireAdmin()) { server.send(403, "text/plain", "Forbidden"); return; }

    if (server.hasArg("username") && server.hasArg("role")) {
        String username = server.arg("username");
        int newRole = server.arg("role").toInt();

        Session* sess = getCurrentSession();
        
        // Fetch target user's current role
        int targetRole = -1;
        sqlite3_stmt *stmtRole;
        if (sqlite3_prepare_v2(db, "SELECT ROLE FROM USERS WHERE USERNAME = ?;", -1, &stmtRole, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmtRole, 1, username.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmtRole) == SQLITE_ROW) targetRole = sqlite3_column_int(stmtRole, 0);
            sqlite3_finalize(stmtRole);
        }

        if (targetRole == -1) { server.send(404, "text/plain", "User not found"); return; }

        // Security: Cannot modify someone of equal or higher rank, and cannot promote someone to your rank or higher
        if (targetRole >= sess->role) {
            server.send(403, "text/plain", "Cannot modify users of equal or higher rank"); return;
        }
        if (newRole >= sess->role) {
            server.send(403, "text/plain", "Cannot promote user to your rank or higher"); return;
        }

        sqlite3_stmt *stmt;
        const char *sql = "UPDATE USERS SET ROLE = ? WHERE USERNAME = ?;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, newRole);
            sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);

            if (sqlite3_step(stmt) == SQLITE_DONE) {
                // Remove existing sessions for this user so they have to login again
                for (auto it = activeSessions.begin(); it != activeSessions.end(); ) {
                    if (it->username == username) {
                        it = activeSessions.erase(it);
                    } else {
                        ++it;
                    }
                }
                server.send(200, "text/plain", "Role updated successfully");
            } else {
                server.send(500, "text/plain", "Failed to update role");
            }
            sqlite3_finalize(stmt);
        } else {
             server.send(500, "text/plain", "Database error");
        }
    } else {
        server.send(400, "text/plain", "Missing arguments");
    }
}

void handleAdminUsersDelete() {
    if (!requireAdmin()) { server.send(403, "text/plain", "Forbidden"); return; }

    if (server.hasArg("username")) {
        String username = server.arg("username");

        // Prevent deleting oneself
        Session* sess = getCurrentSession();
        if (sess && sess->username == username) {
            server.send(400, "text/plain", "Cannot delete yourself");
            return;
        }

        // Fetch target user's current role to prevent an Admin from deleting a Master Admin
        int targetRole = -1;
        sqlite3_stmt *stmtRole;
        if (sqlite3_prepare_v2(db, "SELECT ROLE FROM USERS WHERE USERNAME = ?;", -1, &stmtRole, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmtRole, 1, username.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmtRole) == SQLITE_ROW) targetRole = sqlite3_column_int(stmtRole, 0);
            sqlite3_finalize(stmtRole);
        }

        if (targetRole >= sess->role) {
            server.send(403, "text/plain", "Cannot delete users of equal or higher rank");
            return;
        }

        sqlite3_stmt *stmt;
        const char *sql = "DELETE FROM USERS WHERE USERNAME = ?;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_DONE) {
                // Remove existing sessions
                for (auto it = activeSessions.begin(); it != activeSessions.end(); ) {
                    if (it->username == username) {
                        it = activeSessions.erase(it);
                    } else {
                        ++it;
                    }
                }
                server.send(200, "text/plain", "User deleted successfully");
            } else {
                server.send(500, "text/plain", "Failed to delete user");
            }
            sqlite3_finalize(stmt);
        } else {
             server.send(500, "text/plain", "Database error");
        }
    } else {
        server.send(400, "text/plain", "Missing arguments");
    }
}

static int build_logs_json_callback(void *data, int argc, char **argv, char **azColName) {
    String* json = (String*)data;
    if (json->length() > 0) *json += ",";

    String username = argv[0] ? argv[0] : "Unknown";
    String action = argv[1] ? argv[1] : "UNKNOWN";
    String details = argv[2] ? argv[2] : "";
    String timestamp = argv[3] ? argv[3] : "0";

    details.replace("\"", "\\\""); // Escape double quotes inside the JSON string

    *json += "{\"username\":\"" + username + "\",\"action\":\"" + action + "\",\"details\":\"" + details + "\",\"timestamp\":" + timestamp + "}";
    return 0;
}

void handleAdminLogsGet() {
    if (!requireAdmin()) { server.send(403, "text/plain", "Forbidden"); return; }
    String jsonResult = "";
    const char* sql = "SELECT USERNAME, ACTION, DETAILS, TIMESTAMP FROM ACTIVITY_LOG ORDER BY TIMESTAMP DESC LIMIT 100;";
    sqlite3_exec(db, sql, build_logs_json_callback, (void*)&jsonResult, NULL);
    server.send(200, "application/json", "[" + jsonResult + "]");
}

void handleAdminNetwork() {
    if (!requireAdmin()) { server.send(403, "text/plain", "Forbidden"); return; }
    
    String json = "{";
    json += "\"clients\":" + String(WiFi.softAPgetStationNum()) + ",";
    json += "\"mac\":\"" + WiFi.softAPmacAddress() + "\",";
    json += "\"uptime\":" + String(millis()) + ",";
    json += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"totalHeap\":" + String(ESP.getHeapSize());
    json += "}";
    
    server.send(200, "application/json", json);
}

void handleApiPing() {
    // Simple ultra-fast endpoint to measure latency
    server.send(200, "text/plain", "pong");
}

void handleSpeedTestGet() {
    if (!requireAdmin()) { server.send(403, "text/plain", "Forbidden"); return; }
    
    // Send 500KB of dummy data to measure download speed
    server.setContentLength(512000);
    server.send(200, "application/octet-stream", "");
    
    WiFiClient client = server.client();
    uint8_t buf[2048];
    memset(buf, '0', sizeof(buf)); // Fill buffer with zeros
    
    size_t bytesSent = 0;
    while (bytesSent < 512000) {
        size_t toSend = (512000 - bytesSent) > sizeof(buf) ? sizeof(buf) : (512000 - bytesSent);
        client.write(buf, toSend);
        bytesSent += toSend;
        delay(1); // Yield to prevent watchdog crash during tight loop
    }
}

void initWebServer() {

    const char* headerkeys[] = {"Cookie"};
    size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
    server.collectHeaders(headerkeys, headerkeyssize);

    server.on("/login", HTTP_POST, handleLogin);
    server.on("/register", HTTP_POST, handleRegister);
    server.on("/api/logout", HTTP_POST, handleLogout);
    server.on("/", handleRoot);
    server.on("/api/list", HTTP_GET, handleApiList);
    server.on("/api/search", HTTP_GET, handleApiSearch);
    server.on("/download", HTTP_GET, handleDownload);
    server.on("/delete", HTTP_GET, handleDelete);
    server.on("/api/create_folder", HTTP_POST, handleCreateFolder);
    server.on("/api/rename", HTTP_POST, handleRename);

    // Open endpoint so the browser can sync time before logging in
    server.on("/api/time", HTTP_POST, handleTimeSync);

    server.on("/upload", HTTP_POST, []() {
        if (!requireWriteAccess()) { server.send(401, "text/plain", "Unauthorized"); return; }
        server.send(200, "text/plain", "Upload complete");
    }, handleUpload);

    // Network and diagnostic routes
    server.on("/api/ping", HTTP_GET, handleApiPing);
    server.on("/api/speedtest", HTTP_GET, handleSpeedTestGet);
    server.on("/api/speedtest", HTTP_POST, []() {
        if (!requireAdmin()) { server.send(403, "text/plain", "Forbidden"); return; }
        server.send(200, "text/plain", "Upload speed test complete");
    }, []() { HTTPUpload& upload = server.upload(); }); // Empty upload handler to consume and discard dummy data


    server.on("/api/me", HTTP_GET, handleApiMe);
    server.on("/api/admin/users", HTTP_GET, handleAdminUsersGet);
    server.on("/api/admin/roles", HTTP_POST, handleAdminRolesPost);
    server.on("/api/admin/users", HTTP_DELETE, handleAdminUsersDelete);
    server.on("/api/admin/logs", HTTP_GET, handleAdminLogsGet);
    server.on("/api/admin/network", HTTP_GET, handleAdminNetwork);

    server.onNotFound(handleStaticWebFiles);

    server.begin();
    Serial.println("Web Server Ready. Go to 192.168.4.1");
}

void handleClient() {
    server.handleClient();
}