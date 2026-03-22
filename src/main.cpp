#include <Arduino.h>
#include <WiFi.h>

#include "StorageManager.h"
#include "DatabaseManager.h"
#include "WebServerManager.h"

const char* ssid = "Esp32_Server";
const char* password = "HelloWorld124";

void setup() {
    Serial.begin(115200);
    delay(2000);

    WiFi.mode(WIFI_AP);
    WiFi.setTxPower(WIFI_POWER_11dBm);
    WiFi.softAP(ssid, password);

    if (!initStorage()) {
        Serial.println("SD Card Mount Failed!");
        return; 
    }

    if (!initDatabase()) {
        Serial.println("Failed to open SQLite database.");
    }

    initWebServer();
}

void loop() {
    handleClient();
}