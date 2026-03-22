#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <FS.h>

extern WebServer server;
extern File fsUploadFile;

void initWebServer();
void handleClient();
