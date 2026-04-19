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
    Serial.begin(115200); //sets polling rate (bps) for data viewing
    delay(2000); //allows esp32 enough time to provide power to components

    //Hosts wifi access point and sets appropriate power usage
    WiFi.mode(WIFI_AP);
    WiFi.setTxPower(WIFI_POWER_11dBm);
    WiFi.softAP(ssid, password); //defiens access point name and password

    //Mounts both internal flash and SD card, prints error and halts if either fails
    if (!initStorage()) {
        return; 
    }

    // Creates and Checks if instance of Database object from SQLite library exists
    if (!initDatabase()) {
        Serial.println("Failed to open SQLite database.");
    }
    //Tells the webserver instance from webServerManager.cpp to wake
    //runs server.begin(), telling esp32 to officially open port 80 on the access point and listen for incoming http reqeusts
    //links URLs to c++ functions like: /upload and /api/list
    initWebServer();
}

//This is the main loop. runs continuously.
void loop() {
    //actively checks for incoming http request.
    //When the function looks at a url request it matches to appropriate routes in initWebServer()
    //if user asked for /api/list, handleClient() pauses the loop and instead executes the handleAPIList() c++ function
    //if User asks for /upload, the handleUpload() function is called
    handleClient();
}