#include "WebServerManager.h"
#include "StorageManager.h"
#include "DatabaseManager.h"
#include "SD_MMC.h"
#include <vector>

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
    return sess != nullptr && sess->role == 1;
}

//Processes the form submission from a user login attempt.
void handleLogin() {
    if (server.hasArg("username") && server.hasArg("password")) {
        String username = server.arg("username");
        String password = server.arg("password");

        username.trim();
        password.trim();

        int role = verifyUser(username, password);
        if (role != -1) {
            // Generate a random session token upon successful login
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
        // Default role is 0 (standard user)
        if (createUser(username, password, 0)) {
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

        if (lowerPath == "/index.db" || lowerPath == "/index.db-journal" || lowerPath == "/index.html" || lowerPath == "/login.html" || lowerPath.indexOf("system volume information") != -1) {
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
    if (!isAuthenticated()) { server.send(401, "text/plain", "Unauthorized"); return; }

    if (server.hasArg("file")) {
        String path = server.arg("file");
        if (!path.startsWith("/")) path = "/" + path;

        String lowerPath = path;
        lowerPath.toLowerCase();

        if (lowerPath == "/index.db" || lowerPath == "/index.db-journal" || lowerPath == "/index.html" || lowerPath == "/login.html" || lowerPath.indexOf("system volume information") != -1) {
            server.send(403, "text/plain", "Forbidden: Cannot delete system files");
            return;
        }

        if (deleteFileOrFolder(path)) {
            server.send(200, "text/plain", "Deleted successfully");
            return;
        }
    }

    server.send(500, "text/plain", "Failed to delete");
}

void handleUpload() {
    if (!isAuthenticated()) return;

    HTTPUpload& upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
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
            Serial.printf("Upload Complete: %s, Size: %u bytes\n", upload.filename.c_str(), upload.totalSize);
        }
    }
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

    *json += "{\"username\":\"" + username + "\",\"role\":" + role + "}";
    return 0;
}

void handleAdminUsersGet() {
    if (!requireAdmin()) { server.send(403, "text/plain", "Forbidden"); return; }

    String jsonResult = "";
    const char* sql = "SELECT USERNAME, ROLE FROM USERS;";
    sqlite3_exec(db, sql, build_users_json_callback, (void*)&jsonResult, NULL);

    server.send(200, "application/json", "[" + jsonResult + "]");
}

void handleAdminRolesPost() {
    if (!requireAdmin()) { server.send(403, "text/plain", "Forbidden"); return; }

    if (server.hasArg("username") && server.hasArg("role")) {
        String username = server.arg("username");
        int newRole = server.arg("role").toInt();

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


void initWebServer() {

    const char* headerkeys[] = {"Cookie"};
    size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
    server.collectHeaders(headerkeys, headerkeyssize);

    server.on("/login", HTTP_POST, handleLogin);
    server.on("/register", HTTP_POST, handleRegister);
    server.on("/", handleRoot);
    server.on("/api/list", HTTP_GET, handleApiList);
    server.on("/api/search", HTTP_GET, handleApiSearch);
    server.on("/download", HTTP_GET, handleDownload);
    server.on("/delete", HTTP_GET, handleDelete);

    server.on("/upload", HTTP_POST, []() {
        if (!isAuthenticated()) { server.send(401, "text/plain", "Unauthorized"); return; }
        server.send(200, "text/plain", "Upload complete");
    }, handleUpload);

    server.on("/api/me", HTTP_GET, handleApiMe);
    server.on("/api/admin/users", HTTP_GET, handleAdminUsersGet);
    server.on("/api/admin/roles", HTTP_POST, handleAdminRolesPost);
    server.on("/api/admin/users", HTTP_DELETE, handleAdminUsersDelete);

    server.onNotFound(handleStaticWebFiles);

    server.begin();
    Serial.println("Web Server Ready. Go to 192.168.4.1");
}

void handleClient() {
    server.handleClient();
}