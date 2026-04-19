#include "StorageManager.h"
#include <LittleFS.h>



//mounts internal flash and appropriate pins for microSSD component
bool initStorage() {
    bool success = true;

    // Mount the internal flash memory for system files
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS Mount Failed!");
        success = false;
    }

    //uses dedicated physical pins for msd card
    SD_MMC.setPins(39, 38, 40);
    //   "/sdcard" becomes the root path and
    //Maps the physical root to the /sdcard directory in the virtual file system.
    if (!SD_MMC.begin("/sdcard", true)) {
        Serial.println("SD Card Mount Failed!");
        success = false;
    }
    return success;
}

//takes a directory/folder sent from user in URL format to delete the contents inside it
bool deleteFolderRecursive(String dirPath) {
    //creates a folder object that stores the contents of the directory from the microsd
    File dir = SD_MMC.open(dirPath);
    //checks if the opened object exists and if it is a directory
    if (!dir || !dir.isDirectory()) return false;

    //opens the first file in the directory and stores it in a file object
    File file = dir.openNextFile();
    //This function traverses the files within a root, performing a depth-first search through recursion.
    while (file) {

        //strores the path of the object as a string
        String itemPath = file.path();
        //checks if it is a file or folder
        bool isDir = file.isDirectory();
        //closes the file object
        file.close();



        // if the file is a folder it will then search the elements inside the new found child folder first.
        //else it deletes if the object is a file using the file path stored previously.
        if (isDir) {
            deleteFolderRecursive(itemPath);
        } else {
            SD_MMC.remove(itemPath);
        }
        //rewinds, meaning to return to the beginning of the directory because when an element is removed
        //all elements shift down one index, meaning the first index will eventually hold the last element
        //by continoulsy deleting the first element and accessing it using opeNextFile()
        dir.rewindDirectory();
        file = dir.openNextFile();
    }
    dir.close();
    //remove directory
    return SD_MMC.rmdir(dirPath);
}

//this functions combines folder deletion and file deletion into a single function.
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
