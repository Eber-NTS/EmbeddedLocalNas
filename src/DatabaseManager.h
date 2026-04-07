#pragma once
#include <Arduino.h>
#include <sqlite3.h>

extern sqlite3 *db;

bool initDatabase();
void indexInternalDrive(String targetDir);
bool createUser(String username, String password);
bool verifyUser(String username, String password);
