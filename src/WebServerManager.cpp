#include "WebServerManager.h"
#include "StorageManager.h"
#include "DatabaseManager.h"
#include "SD_MMC.h"

WebServer server(80);
File fsUploadFile;


const char* WEB_PASSWORD = "admin";
String currentSessionCookie = "";

//verifies if incoming http request is coming from a user who had already logged in before
bool isAuthenticated() {
    //when browsers make a request, it sends http headers. If the user had previously logged in, the request will attach
    //a cookie header (given from handleLogin function).
    if (server.hasHeader("Cookie")) {
        String cookie = server.header("Cookie");

        //checks if currenSessionCookie exists and is not the default empty string
        if (currentSessionCookie.length() > 0) {

            int startIndex = cookie.indexOf(currentSessionCookie);
            if (startIndex != -1) {
                // Ensure the match is exact and not just a prefix of another value
                int endIndex = startIndex + currentSessionCookie.length();
                if (endIndex == cookie.length() || cookie[endIndex] == ';') {
                    return true;
                }
            }
        }
    }
    return false;
}

    //Processes the form submission from a user login attempt. Determines if they are authorized and redirects them accordingly
void handleLogin() {
    //Checks if the browser actually sent a form field named "password". And Checks if the password they typed
    //matches the WEB_password, which is hardcoded for right now.
    if (server.hasArg("password") && server.arg("password") == WEB_PASSWORD) {
        // Generate a random session token upon successful login
        currentSessionCookie = "ESP32_SESSION=" + String(esp_random());

        //sends data to user's web Server to save the random token as a cookie.
        server.sendHeader("Set-Cookie", currentSessionCookie + "; Path=/; HttpOnly");
        //redirects user after successful form submission. homepage located at '/'
        server.sendHeader("Location", "/");
        server.send(303);
    } else {
        //redirects user back to login page. error=1 is used in login.html to reveal an error message.
        server.sendHeader("Location", "/login.html?error=1");
        server.send(303);
    }
}

//data container used to pass state between web server routing functions and the SQLite database callback function
//this object stores the JSON packet data used for building the list of files appropriate to web directory.
struct RenderContext {
    String currentPath;
    String generatedHTML;
};

//callback function for SQLite. Takes a row of data from the db, formats it as JSON object
//and appends it to a long string that will be sent back to the browser

//*data is a generic pointer. argc is the number of columns returned in the current row. Char **argv is an array of strings
//containing the actual data for each column in the row
//char **azColName is an array of strings containing the column names (Name, is_Folder)
static int build_json_callback(void *data, int argc, char **argv, char **azColName) {
    RenderContext* ctx = (RenderContext*)data;

    //add comma if string already has data
    if (ctx->generatedHTML.length() > 0) ctx->generatedHTML += ",";

    //data extraction
    String name = argv[0] ? argv[0] : "Unknown";
    String isFolder = argv[1] ? argv[1] : "0";
    String size = argv[2] ? argv[2] : "0";
    String parentDir = (argc > 3 && argv[3]) ? argv[3] : "";

    //contructs the JSON object using string concatanation
    ctx->generatedHTML += "{\"name\":\"" + name + "\",\"isFolder\":" + isFolder + ",\"size\":" + size;
    if (parentDir.length() > 0) {
        ctx->generatedHTML += ",\"parentDir\":\"" + parentDir + "\"";
    }
    ctx->generatedHTML += "}";
    //signals SQLite to run this callback again for next row
    return 0;
}

//Handler for main homepage. Executes when user types esp32's IP address into their browser
void handleRoot() {
    //checks if user is authenticated before providing access to homepage
    if (!isAuthenticated()) {
        //sends user to login page
        server.sendHeader("Location", "/login.html");
        server.send(303);
        return;
    }
    //finds index.html and streams it to user's browser
    if (SD_MMC.exists("/index.html")) {
        File file = SD_MMC.open("/index.html", "r");
        server.streamFile(file, "text/html");
        file.close();
    } else {
        server.send(404, "text/plain", "Missing index.html on drive. Please upload it.");
    }
}

//primary api endpoint the web interface calls when it needs to display the contents of a folder.
//finds which folder the user wants to view, indexes the folder into the db, and returs a formatted JSON list of the
//contents of the folder inside.
void handleApiList() {
    //rejects the request if attempted by a user who is not authenticated
    if (!isAuthenticated()) { server.send(401, "text/plain", "Unauthorized"); return; }

    //creates an instance of RenderContext struct
    RenderContext context;
    context.generatedHTML = "";

    //checks if the dir argument was provided. If it was, it sets currentPath to that folder.
    context.currentPath = server.hasArg("dir") ? server.arg("dir") : "/";
    indexInternalDrive(context.currentPath);

    //char variable of instructions used to extract meta data from msd
    char *query = sqlite3_mprintf("SELECT NAME, IS_FOLDER, SIZE FROM FILES WHERE PARENT_DIR='%q' ORDER BY IS_FOLDER DESC, NAME ASC;", context.currentPath.c_str());
    sqlite3_exec(db, query, build_json_callback, (void*)&context, NULL);
    sqlite3_free(query);

    //
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

        // Security: Prevent downloading core system files
        if (path == "/index.db" || path == "/index.html" || path == "/login.html") {
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

        //Prevent deleting core system files
        if (path == "/index.db" || path == "/index.html" || path == "/login.html") {
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
