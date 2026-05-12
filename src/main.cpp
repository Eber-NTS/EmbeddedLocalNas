#include <Arduino.h> //Import Arduino library
#include <WiFi.h> //Import library for hosting http access point


#include "StorageManager.h"
#include "DatabaseManager.h"
#include "WebServerManager.h"

//Static wifi credentials
const char* ssid = "Esp32_Server";
const char* password = "helloworld";


//Setup function, runs once before main loop.
void setup() {
    Serial.begin(115200); //sets polling rate (bps). This is used for viewing the input/output monitor.
    delay(2000); //provides esp32 enough time to provide power to components

    //Hosts wifi access point and sets appropriate power
    WiFi.mode(WIFI_AP);
    WiFi.setTxPower(WIFI_POWER_17dBm);
    WiFi.softAP(ssid, password); //defines access point name and password

    //Mounts the card, if it fails it returns a failed message.
    if (!initStorage()) {
        Serial.println("SD Card Mount Failed!");
        return; 
    }

    // Creates and Checks if instance of Database object from SQLite library exists
    if (!initDatabase()) {
        Serial.println("Failed to open SQLite database.");
    }
    //Tells the webserver instance from webServerManager.cpp to wake
    //runs server.begin(), telling esp32 to officially open port 80 on the access point and listen for incoming http reqeust
    initWebServer();
}

//This is the main loop.
void loop() {
    // Actively listen for incoming HTTP requests from connected browsers
    // If a request is found, pause this loop and execute the linked route (e.g., handleUpload)
    //PROCESS incoming web client requests()
    handleClient();
    delay(1); // Yields CPU time to the background Wi-Fi radio tasks, preventing packet loss
}