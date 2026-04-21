#pragma once
#include <Arduino.h>
#include <sqlite3.h>

extern sqlite3 *db;

bool initDatabase();
void indexInternalDrive(String targetDir);
bool createUser(String username, String password, int role = 0);
int verifyUser(String username, String password);