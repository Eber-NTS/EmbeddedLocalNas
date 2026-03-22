#include "WebServerManager.h"
#include "StorageManager.h"
#include "DatabaseManager.h"
#include "SD_MMC.h"

WebServer server(80);
File fsUploadFile;

struct RenderContext {
    String currentPath;
    String generatedHTML;
};

static int build_html_callback(void *data, int argc, char **argv, char **azColName) {
    RenderContext* ctx = (RenderContext*)data;

    String name = argv[0] ? argv[0] : "Unknown";
    String isFolder = argv[1] ? argv[1] : "0";
    String size = argv[2] ? argv[2] : "0";

    String icon = (isFolder == "1") ? "📁" : "📄";
    String sizeText = (isFolder == "1") ? "--" : String(size.toInt() / 1024) + " KB";

    String itemPath = ctx->currentPath;
    if (!itemPath.endsWith("/")) itemPath += "/";
    itemPath += name;

    String primaryAction;
    if (isFolder == "1") {
        primaryAction = "<a href='/?dir=" + itemPath + "' style='color: #fce84d; text-decoration: none; margin-right: 15px;'>Open</a>";
    } else {
        primaryAction = "<a href='/download?file=" + itemPath + "' style='color: #4daafc; text-decoration: none; margin-right: 15px;'>Download</a>";
    }
    String deleteAction = "<a href='/delete?file=" + itemPath + "&dir=" + ctx->currentPath + "' style='color: #ff4d4d; text-decoration: none;' onclick=\"return confirm('Delete " + name + "?');\">Delete</a>";

    ctx->generatedHTML += "<div style='display: flex; justify-content: space-between; align-items: center; padding: 12px; border-bottom: 1px solid #333;'>";
    ctx->generatedHTML +=   "<div style='flex: 2; display: flex; align-items: center;'><span style='font-size: 24px; margin-right: 15px;'>" + icon + "</span>" + name + "</div>";
    ctx->generatedHTML +=   "<div style='flex: 1; color: #aaa;'>" + sizeText + "</div>";
    ctx->generatedHTML +=   "<div style='flex: 1; text-align: right;'>" + primaryAction + deleteAction + "</div>";
    ctx->generatedHTML += "</div>";

    return 0;
}

static int build_json_callback(void *data, int argc, char **argv, char **azColName) {
    RenderContext* ctx = (RenderContext*)data;

    if (ctx->generatedHTML.length() > 0) ctx->generatedHTML += ",";

    String name = argv[0] ? argv[0] : "Unknown";
    String isFolder = argv[1] ? argv[1] : "0";
    String size = argv[2] ? argv[2] : "0";

    // Build a JSON object for this file/folder
    ctx->generatedHTML += "{\"name\":\"" + name + "\",\"isFolder\":" + isFolder + ",\"size\":" + size + "}";

    return 0;
}

String getParentDir(String path) {
    if (path == "/" || path == "") return "/";
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash == 0) return "/";
    return path.substring(0, lastSlash);
}

void handleRoot() {
    String clientIP = server.client().remoteIP().toString();
    RenderContext context;
    context.generatedHTML = "";

    context.currentPath = server.hasArg("dir") ? server.arg("dir") : "/";
    indexInternalDrive(context.currentPath);

    char *query = sqlite3_mprintf("SELECT NAME, IS_FOLDER, SIZE FROM FILES WHERE PARENT_DIR='%q' ORDER BY IS_FOLDER DESC, NAME ASC;", context.currentPath.c_str());
    sqlite3_exec(db, query, build_html_callback, (void*)&context, NULL);
    sqlite3_free(query);

    String html = "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>ESP32 NAS</title>";
    html += "<style>body { background-color: #202020; color: #ffffff; font-family: 'Segoe UI', sans-serif; margin: 0; padding: 20px; } .container { max-width: 800px; margin: auto; } .file-box { background-color: #2d2d2d; border-radius: 8px; padding: 10px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); margin-bottom: 20px;} .upload-box { background-color: #2d2d2d; border-radius: 8px; padding: 15px; text-align: center; } input[type='file'] { color: white; margin-right: 10px; } button { background-color: #4daafc; color: #000; border: none; padding: 8px 16px; border-radius: 4px; cursor: pointer; font-weight: bold; }</style></head><body>";

    html += "<div class='container'>";
    html +=   "<div style='border-bottom: 1px solid #444; padding-bottom: 10px; margin-bottom: 20px; display: flex; justify-content: space-between; align-items: baseline;'>";
    html +=     "<h2>Index of: " + context.currentPath + "</h2>";
    html +=     "<span style='color: #aaa; font-size: 14px;'>IP: " + clientIP + "</span>";
    html +=   "</div>";

    html +=   "<div class='file-box'>";

    if (context.currentPath != "/") {
        String parent = getParentDir(context.currentPath);
        html += "<div style='padding: 12px; border-bottom: 1px solid #333;'>";
        html += "📁 <a href='/?dir=" + parent + "' style='color: #fff; text-decoration: none; margin-left: 15px;'><b>.. (Up one level)</b></a>";
        html += "</div>";
    }

    if (context.generatedHTML == "") {
        html += "<p style='color: #888; text-align: center; padding: 10px;'>Folder is empty.</p>";
    } else {
        html += context.generatedHTML;
    }
    html +=   "</div>";

    html += "<div class='upload-box'>";
    html +=   "<h3>Upload File to Current Folder</h3>";
    html +=   "<form method='POST' action='/upload?dir=" + context.currentPath + "' enctype='multipart/form-data'>";
    html +=     "<input type='file' name='upload_file' required>";
    html +=     "<button type='submit'>Upload File</button>";
    html +=   "</form>";
    html += "</div>";

    html += "</div></body></html>";
    server.send(200, "text/html", html);
}

void handleApiList() {
    RenderContext context;
    context.generatedHTML = "";

    context.currentPath = server.hasArg("dir") ? server.arg("dir") : "/";
    indexInternalDrive(context.currentPath);

    char *query = sqlite3_mprintf("SELECT NAME, IS_FOLDER, SIZE FROM FILES WHERE PARENT_DIR='%q' ORDER BY IS_FOLDER DESC, NAME ASC;", context.currentPath.c_str());
    sqlite3_exec(db, query, build_json_callback, (void*)&context, NULL);
    sqlite3_free(query);

    // Wrap the array of files inside a main JSON object
    String json = "{\"dir\":\"" + context.currentPath + "\",\"files\":[" + context.generatedHTML + "]}";
    
    server.send(200, "application/json", json);
}

void handleDownload() {
    if (server.hasArg("file")) {
        String path = server.arg("file");
        if (!path.startsWith("/")) path = "/" + path;

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
    String path = server.uri(); // Gets the requested path, e.g., "/style.css"
    
    if (SD_MMC.exists(path)) {
        File file = SD_MMC.open(path, "r");
        server.streamFile(file, getContentType(path));
        file.close();
        return;
    }
    server.send(404, "text/plain", "404: File Not Found");
}

void handleDelete() {
    if (server.hasArg("file")) {
        String path = server.arg("file");
        if (!path.startsWith("/")) path = "/" + path;

        if (SD_MMC.exists(path)) {
            File f = SD_MMC.open(path);
            bool isDir = f.isDirectory();
            f.close();

            if (isDir) {
                deleteFolderRecursive(path);
            } else {
                SD_MMC.remove(path);
            }
        }
    }

    String redir = server.hasArg("dir") ? server.arg("dir") : "/";
    server.sendHeader("Location", "/?dir=" + redir);
    server.send(303);
}

void handleUpload() {
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
    server.on("/", handleRoot);
    server.on("/api/list", HTTP_GET, handleApiList);
    server.on("/download", HTTP_GET, handleDownload);
    server.on("/delete", HTTP_GET, handleDelete);

    server.on("/upload", HTTP_POST, []() {
        String redir = server.hasArg("dir") ? server.arg("dir") : "/";
        server.sendHeader("Location", "/?dir=" + redir);
        server.send(303);
    }, handleUpload);

    // If the route isn't defined above, check if it's a file on the drive
    server.onNotFound(handleStaticWebFiles);

    server.begin();
    Serial.println("Web Server Ready. Go to 192.168.4.1");
}

void handleClient() {
    server.handleClient();
}