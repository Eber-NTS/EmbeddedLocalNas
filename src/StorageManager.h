#pragma once
#include <Arduino.h>
#include "SD_MMC.h"
#include <LittleFS.h>

bool initStorage();
bool deleteFolderRecursive(String dirPath);
bool deleteFileOrFolder(String path);
