#include "StorageManager.h"

//mounts appropriate pins for microSSD component
bool initStorage() {

    SD_MMC.setPins(39, 38, 40);
    if (!SD_MMC.begin("/sdcard", true)) {
        return false;
    }
    return true;
}

bool deleteFolderRecursive(String dirPath) {
    File dir = SD_MMC.open(dirPath);
    if (!dir || !dir.isDirectory()) return false;

    File file = dir.openNextFile();
    while (file) {
        String itemPath = file.path();
        bool isDir = file.isDirectory();
        file.close();

        if (isDir) {
            deleteFolderRecursive(itemPath);
        } else {
            SD_MMC.remove(itemPath);
        }

        dir.rewindDirectory();
        file = dir.openNextFile();
    }
    dir.close();

    return SD_MMC.rmdir(dirPath);
}

bool deleteFileOrFolder(String path) {
    if (!SD_MMC.exists(path)) {
        return false;
    }

    File f = SD_MMC.open(path);
    bool isDir = f.isDirectory();
    f.close();

    if (isDir) {
        return deleteFolderRecursive(path);
    } else {
        return SD_MMC.remove(path);
    }
}
