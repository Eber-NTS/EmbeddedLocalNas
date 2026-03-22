#pragma once
#include <Arduino.h>
#include "SD_MMC.h"

bool initStorage();
bool deleteFolderRecursive(String dirPath);
