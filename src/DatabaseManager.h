#pragma once
#include <Arduino.h>
#include <sqlite3.h>

extern sqlite3 *db;

bool initDatabase();
void indexInternalDrive(String targetDir);
bool createUser(String username, String password, int role = 1);
int verifyUser(String username, String password);
void logActivity(String username, String action, String details);