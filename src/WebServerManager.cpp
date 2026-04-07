#include "WebServerManager.h"
#include "StorageManager.h"
#include "DatabaseManager.h"
#include "SD_MMC.h"
#include <vector>

WebServer server(80);
File fsUploadFile;

// Store multiple active session cookies
std::vector<String> activeSessions;

//verifies if incoming http request is coming from a user who had already logged in before
bool isAuthenticated() {
    if (server.hasHeader("Cookie")) {
        String cookie = server.header("Cookie");

        for (const String& session : activeSessions) {
            int startIndex = cookie.indexOf(session);
            if (startIndex != -1) {
                // Ensure the match is exact and not just a prefix of another value
                int endIndex = startIndex + session.length();
                if (endIndex == cookie.length() || cookie[endIndex] == ';') {
                    return true;
                }
            }
        }
    }
    return false;
}

//Processes the form submission from a user login attempt.
void handleLogin() {
    if (server.hasArg("username") && server.hasArg("password")) {
        String username = server.arg("username");
        String password = server.arg("password");

        if (verifyUser(username, password)) {
            // Generate a random session token upon successful login
            String newSession = "ESP32_SESSION=" + String(esp_random());
            activeSessions.push_back(newSession);

            server.sendHeader("Set-Cookie", newSession + "; Path=/; HttpOnly");
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

        // Attempt to create user (will fail if username exists due to PRIMARY KEY)
        if (createUser(username, password)) {
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

    ctx->generatedHTML += "{\"name\":\"" + name + "\",\"isFolder\":" + isFolder + ",\"size\":" + size;
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
    indexInternalDrive(context.currentPath);

    char *query = sqlite3_mprintf("SELECT NAME, IS_FOLDER, SIZE FROM FILES WHERE PARENT_DIR='%q' ORDER BY IS_FOLDER DESC, NAME ASC;", context.currentPath.c_str());
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

    char *query = sqlite3_mprintf("SELECT NAME, IS_FOLDER, SIZE, PARENT_DIR FROM FILES WHERE NAME LIKE '%%%q%%' ORDER BY IS_FOLDER DESC, NAME ASC LIMIT 100;", queryStr.c_str());
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

        if (lowerPath == "/index.db" || lowerPath == "/index.db-journal" || lowerPath == "/index.html" || lowerPath == "/login.html") {
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
    if (path != "/login.html" && !isAuthenticated()) {
        server.sendHeader("Location", "/login.html");
        server.send(303);
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

        if (lowerPath == "/index.db" || lowerPath == "/index.db-journal" || lowerPath == "/index.html" || lowerPath == "/login.html") {
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

    server.onNotFound(handleStaticWebFiles);

    server.begin();
    Serial.println("Web Server Ready. Go to 192.168.4.1");
}

void handleClient() {
    server.handleClient();
}
