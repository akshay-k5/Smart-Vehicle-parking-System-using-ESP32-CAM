/*
 * Smart Parking System using ESP32-CAM with Parking Lot Status via Arduino Nano
 * 
 * Features:
 * - WiFi connectivity for HTTPS communication with a remote server.
 * - Camera for license plate recognition with flash light (GPIO 4).
 * - NTP for accurate IST timekeeping.
 * - Web server with UI (1-second refresh) for monitoring parking status, time, and history.
 * - Entry detection via IR sensor (GPIO 13) with 10-second countdown timer.
 * - Manual exit via "Exit" buttons in the database table, confirmation on same page.
 * - Parking lot status from Arduino Nano via UART (GPIO 1, 3).
 * - Visual parking lot status (red=occupied, green=available).
 * - Logs valid number plates with timestamps in database.
 * 
 * Updates:
 * - Removed all servo-related code (library, variables, functions, and logic).
 * - Retained 10-second vehicle movement delay with countdown timer.
 * - Removed "Manual Exit" dropdown section (already removed in previous version).
 * - Fixed manual exit to stay on main page with confirmation message.
 * - Fixed removeFromHistory() to correctly remove Entry records.
 */

// Libraries
#include <Arduino.h>
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <WebServer.h>
#include <soc/soc.h>
#include <soc/rtc_cntl_reg.h>
#include <esp_camera.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <vector>

// WiFi Credentials
const char* ssid = "Your_ssid";
const char* password = "Your_password";

// Server Configuration
const char* serverName = "www.circuitdigest.cloud";
const char* serverPath = "/readnumberplate";
const int serverPort = 443;
const String apiKey = "AjILFDbo37O4";
const String imageViewLinkBase = "https://www.circuitdigest.cloud/static/";
const int flashLight = 4;

// Camera Pins
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

// Hardware Configuration
const int inSensor = 13;
const int RX_PIN = 3;
const int TX_PIN = 1;

// Time Configuration
const char* ntpServer = "pool.ntp.org";
const long utcOffsetInSeconds = 19800; // IST (UTC + 5:30)

// Timing Constants
const int vehicleMoveDelay = 10000; // 10 seconds total delay for vehicle to move
const int siteRefreshTime = 1; // seconds
const unsigned long debounceDelay = 500; // ms
const size_t maxHistorySize = 50;
const unsigned long exitMessageTimeout = 5000; // ms

// Global Objects
NetworkClientSecure client;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, ntpServer, utcOffsetInSeconds);
WebServer server(80);

// Global Variables
String recognizedPlate = "";
String imageLink = imageViewLinkBase + apiKey + ".jpeg";
String currentStatus = "Idle";
String currentTime = "";
String exitMessage = "";
unsigned long exitMessageTime = 0;
int availableSpaces = 3;
int vehicleCount = 0;
bool lot1Occupied = false;
bool lot2Occupied = false;
bool lot3Occupied = false;
String uartBuffer = "";
unsigned long lastEntryTrigger = 0;
unsigned long lastUARTRead = 0;
int imageCount = 0;
int countdown = 0;
unsigned long countdownStart = 0;
bool isEntryCooldown = false; // Flag to disable IR triggers during delay

// Plate History Structure
struct PlateEntry {
    String plateNumber;
    String time;
    String status; // "Entry" or "Exit"
};
std::vector<PlateEntry> plateHistory;

// Function Prototypes
String extractJsonStringValue(const String& jsonString, const String& key);
void parseUARTData(const String& data);
void handleRoot();
void handleTrigger();
void handleExit();
void handleClearHistory();
int sendPhoto();
void addToHistory(const String& plate, const String& status);
void clearHistory();
bool removeFromHistory(const String& plate);

// Extract JSON string value
String extractJsonStringValue(const String& jsonString, const String& key) {
    int keyIndex = jsonString.indexOf(key);
    if (keyIndex == -1) return "";
    int startIndex = jsonString.indexOf(':', keyIndex) + 2;
    int endIndex = jsonString.indexOf('"', startIndex);
    if (startIndex == -1 || endIndex == -1) return "";
    return jsonString.substring(startIndex, endIndex);
}

// Parse UART data from Nano (format: "1,0,1")
void parseUARTData(const String& data) {
    Serial.print("Received UART data: '"); Serial.print(data); Serial.println("'");
    Serial.flush();
    int firstComma = data.indexOf(',');
    int secondComma = data.indexOf(',', firstComma + 1);
    if (firstComma != -1 && secondComma != -1) {
        lot1Occupied = data.substring(0, firstComma).toInt() == 1;
        lot2Occupied = data.substring(firstComma + 1, secondComma).toInt() == 1;
        lot3Occupied = data.substring(secondComma + 1).toInt() == 1;
        vehicleCount = (lot1Occupied ? 1 : 0) + (lot2Occupied ? 1 : 0) + (lot3Occupied ? 1 : 0);
        if (vehicleCount > availableSpaces) vehicleCount = availableSpaces;
        Serial.println("Parsed: Lot1=" + String(lot1Occupied) + ", Lot2=" + String(lot2Occupied) + ", Lot3=" + String(lot3Occupied) + ", vehicleCount=" + String(vehicleCount));
    } else {
        lot1Occupied = lot2Occupied = lot3Occupied = false;
        vehicleCount = 0;
        Serial.println("Invalid UART data: '" + data + "'");
    }
    Serial.flush();
}

// Add entry to plate history
void addToHistory(const String& plate, const String& status) {
    if (plateHistory.size() >= maxHistorySize) {
        plateHistory.erase(plateHistory.begin());
    }
    PlateEntry entry;
    entry.plateNumber = plate;
    entry.time = currentTime;
    entry.status = status;
    plateHistory.push_back(entry);
    Serial.println("Added to database: " + entry.plateNumber + " with status: " + entry.status + " at " + entry.time);
    Serial.flush();
}

// Remove entry from history when exiting
bool removeFromHistory(const String& plate) {
    Serial.println("Attempting to remove plate: " + plate + " with status: Entry");
    for (auto it = plateHistory.begin(); it != plateHistory.end(); ++it) {
        Serial.println("Checking plate: " + it->plateNumber + ", status: " + it->status);
        if (it->plateNumber == plate && it->status == "Entry") {
            plateHistory.erase(it);
            Serial.println("Removed plate: " + plate + " with status: Entry from history");
            Serial.flush();
            return true;
        }
    }
    Serial.println("Plate: " + plate + " with status: Entry not found in history");
    Serial.flush();
    return false;
}

// Clear plate history
void clearHistory() {
    plateHistory.clear();
    Serial.println("Plate history cleared");
    Serial.flush();
}

// Handle root web page
void handleRoot() {
    String html;
    html.reserve(2048);
    html = "<!DOCTYPE html><html><head>"
           "<meta charset='UTF-8'>"
           "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
           "<title>Smart Parking</title>"
           "<style>"
           "body{font-family:Arial;background:#f4f4f9;margin:0;padding:0;color:#333}"
           "header{text-align:center;padding:10px;background:#0e3d79;color:white}"
           ".container{max-width:1200px;margin:0 auto;padding:10px}"
           "h1,h2{text-align:center;margin:10px 0}"
           "p{margin:5px 0}"
           "table{width:100%;border-collapse:collapse;margin:10px 0}"
           "th,td{padding:5px;border:1px solid #ddd;text-align:left}"
           "tr:nth-child(even){background:#f9f9f9}"
           "a{color:#007bff}"
           ".parking-lots{display:flex;justify-content:center;gap:10px;margin:10px 0}"
           ".lot{width:40px;height:40px;border-radius:50%}"
           ".occupied{background:red}"
           ".available{background:green}"
           "button{padding:5px;font-size:14px;background:#0e3d79;color:white;border:none;cursor:pointer}"
           "button:hover{background:#1a5fb4}"
           ".entry{color:green}"
           ".exit{color:red}"
           ".message{font-size:16px;text-align:center;margin:10px 0}"
           ".success{color:#388e3c}"
           ".error{color:#d32f2f}"
           ".countdown{font-size:20px;color:#d32f2f;text-align:center;margin:10px 0}"
           "</style>";
    if (countdown > 0) {
        html += "<script>"
                "function updateCountdown() {"
                "  var countdownElement = document.getElementById('countdown');"
                "  var seconds = parseInt(countdownElement.innerText);"
                "  if (seconds > 0) {"
                "    countdownElement.innerText = seconds - 1;"
                "    setTimeout(updateCountdown, 1000);"
                "  } else {"
                "    countdownElement.style.display = 'none';"
                "  }"
                "}"
                "window.onload = function() { updateCountdown(); };"
                "</script>";
    }
    html += "<meta http-equiv='refresh' content='1;url=/?t=" + String(millis()) + "'>"
            "</head><body>"
            "<header><h1>Smart Parking ELC S4 (10,17,05,06)</h1></header>"
            "<div class='container'>"
            "<h1>ESP32-CAM Parking</h1>"
            "<p><b>Time:</b> " + currentTime + "</p>"
            "<p><b>IP:</b> " + WiFi.localIP().toString() + "</p>"
            "<p><b>Status:</b> " + currentStatus + "</p>";
    if (countdown > 0) {
        html += "<p class='countdown'>Vehicle Move Forward: <span id='countdown'>" + String(countdown) + "</span> seconds</p>";
    }
    if (exitMessage.length() > 0) {
        String messageClass = exitMessage.startsWith("Error") ? "error" : "success";
        html += "<p class='message " + messageClass + "'>" + exitMessage + "</p>";
    }
    html += "<p><b>Plate:</b> " + (recognizedPlate.length() > 0 ? recognizedPlate : "None") + "</p>"
            "<p><b>Image:</b> <a href='" + imageLink + "' target='_blank'>View</a></p>"
            "<h2>Lot Status</h2>"
            "<div class='parking-lots'>"
            "<div class='lot " + String(lot1Occupied ? "occupied" : "available") + "'></div>"
            "<div class='lot " + String(lot2Occupied ? "occupied" : "available") + "'></div>"
            "<div class='lot " + String(lot3Occupied ? "occupied" : "available") + "'></div>"
            "</div>"
            "<p><b>Spaces:</b> " + String(availableSpaces - vehicleCount) + "</p>"
            "<h2>Database</h2>";
    
    if (plateHistory.empty()) {
        html += "<p>No records.</p>";
    } else {
        html += "<table><tr><th>Plate</th><th>Time</th><th>Status</th><th>Action</th></tr>";
        for (const auto& entry : plateHistory) {
            html += "<tr><td>" + entry.plateNumber + "</td><td>" + entry.time + "</td><td class='" + 
                   (entry.status == "Entry" ? "entry" : "exit") + "'>" + entry.status + "</td>";
            if (entry.status == "Entry") {
                html += "<td><form action='/exit' method='POST'>"
                        "<input type='hidden' name='plateNumber' value='" + entry.plateNumber + "'>"
                        "<button type='submit'>Exit</button></form></td>";
            } else {
                html += "<td>-</td>";
            }
            html += "</tr>";
        }
        html += "</table>";
    }
    
    html += "<p><a href='/clearHistory'>Clear History</a></p>"
            "</div></body></html>";
    server.send(200, "text/html", html);
    Serial.println("handleRoot completed");
    Serial.flush();
}

// Handle image capture trigger
void handleTrigger() {
    currentStatus = "Capturing Image";
    Serial.println("Triggering image capture...");
    Serial.flush();
    int status = sendPhoto();
    switch (status) {
        case -1: currentStatus = "Image Capture Failed"; break;
        case -2: currentStatus = "Server Connection Failed"; break;
        case 1: currentStatus = "No Parking Space Available"; break;
        case 2: currentStatus = "Plate Not Detected [No Entry]"; break;
        default: currentStatus = "Idle"; break;
    }
    server.handleClient();
}

// Handle manual exit
void handleExit() {
    if (server.hasArg("plateNumber") && vehicleCount > 0) {
        String plate = server.arg("plateNumber");
        currentStatus = "Processing Exit for Plate: " + plate;
        Serial.println("Manual exit triggered for plate: " + plate + " at " + String(millis()) + "ms");
        Serial.flush();
        
        if (removeFromHistory(plate)) {
            vehicleCount = max(0, vehicleCount - 1); // Ensure vehicle count doesn't go negative
            Serial.println("Updated vehicleCount after exit: " + String(vehicleCount));
            addToHistory(plate, "Exit");
            exitMessage = "Vehicle " + plate + " has exited successfully.";
            currentStatus = "Idle";
        } else {
            exitMessage = "Error: Vehicle " + plate + " not found in entry records.";
            currentStatus = "Idle";
        }
    } else {
        exitMessage = "Error: No vehicle selected or no vehicles present.";
        currentStatus = "Idle";
    }
    exitMessageTime = millis();
    server.sendHeader("Location", "/");
    server.send(303);
    server.handleClient();
}

// Handle clear history request
void handleClearHistory() {
    clearHistory();
    exitMessage = "History cleared successfully.";
    exitMessageTime = millis();
    server.sendHeader("Location", "/");
    server.send(303);
    server.handleClient();
}

// Capture and send photo
int sendPhoto() {
    digitalWrite(flashLight, HIGH);
    delay(100);
    camera_fb_t* fb = esp_camera_fb_get();
    digitalWrite(flashLight, LOW);
    
    if (!fb) {
        Serial.println("Camera capture failed");
        Serial.flush();
        currentStatus = "Image Capture Failed";
        countdown = 0;
        server.handleClient();
        return -1;
    }
    
    Serial.print("Connecting to server: "); Serial.println(serverName);
    Serial.flush();
    client.setInsecure();
    if (!client.connect(serverName, serverPort)) {
        Serial.println("Connection to server failed");
        Serial.flush();
        esp_camera_fb_return(fb);
        countdown = 0;
        return -2;
    }
    
    imageCount++;
    String filename = apiKey + ".jpeg";
    String boundary = "CircuitDigest";
    String head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"imageFile\"; filename=\"" + filename + "\"\r\nContent-Type: image/jpeg\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";
    
    uint32_t imageLen = fb->len;
    uint32_t totalLen = imageLen + head.length() + tail.length();
    
    client.print(String("POST ") + serverPath + " HTTP/1.1\r\n");
    client.print(String("Host: ") + serverName + "\r\n");
    client.println("Content-Length: " + String(totalLen));
    client.println("Content-Type: multipart/form-data; boundary=" + boundary);
    client.println("Authorization: " + apiKey);
    client.println();
    client.print(head);
    
    currentStatus = "Uploading Image";
    server.handleClient();
    Serial.println("Uploading image...");
    Serial.flush();
    uint8_t* fbBuf = fb->buf;
    size_t fbLen = fb->len;
    size_t chunkSize = 1024;
    
    for (size_t n = 0; n < fbLen; n += chunkSize) {
        size_t toWrite = (n + chunkSize < fbLen) ? chunkSize : fbLen - n;
        client.write(fbBuf + n, toWrite);
    }
    
    client.print(tail);
    esp_camera_fb_return(fb);
    Serial.println("Image sent successfully");
    Serial.flush();
    
    currentStatus = "Waiting for Response";
    server.handleClient();
    
    String response;
    response.reserve(512);
    unsigned long startTime = millis();
    while (client.connected() && millis() - startTime < 10000) {
        if (client.available()) {
            response += (char)client.read();
        }
    }
    
    if (response.isEmpty()) {
        Serial.println("No response from server");
        client.stop();
        countdown = 0;
        return -2;
    }
    
    recognizedPlate = extractJsonStringValue(response, "\"number_plate\"");
    imageLink = extractJsonStringValue(response, "\"view_image\"");
    if (imageLink.isEmpty()) imageLink = imageViewLinkBase + apiKey + ".jpeg";
    
    currentStatus = "Response Received";
    server.handleClient();
    Serial.println("Server response received");
    Serial.println("Recognized Plate: '" + recognizedPlate + "'");
    Serial.println("Image Link: '" + imageLink + "'");
    Serial.flush();
    
    if (vehicleCount >= availableSpaces) {
        Serial.println("No parking space available, vehicleCount=" + String(vehicleCount));
        client.stop();
        countdown = 0;
        return 1;
    }
    
    if (recognizedPlate.isEmpty()) {
        currentStatus = "Plate Not Detected [No Entry]";
        Serial.println(currentStatus);
        client.stop();
        countdown = 0;
        return 2;
    }
    
    if (recognizedPlate.length() > 4 && recognizedPlate.length() <= 12) {
        addToHistory(recognizedPlate, "Entry");
        vehicleCount++;
        if (vehicleCount > availableSpaces) vehicleCount = availableSpaces;
        Serial.println("Updated vehicleCount after entry: " + String(vehicleCount));
        currentStatus = "Vehicle Entering";
        Serial.println("Starting 10-second delay for vehicle entry (IR disabled)");
        Serial.flush();
        isEntryCooldown = true; // Disable IR triggers
        countdown = 10;
        countdownStart = millis();

        // Wait out the 10-second delay for vehicle movement
        while (millis() - countdownStart < vehicleMoveDelay) {
            unsigned long elapsed = (millis() - countdownStart) / 1000;
            int newCountdown = max(0, 10 - (int)elapsed);
            if (newCountdown != countdown) {
                countdown = newCountdown;
                Serial.println("Countdown: " + String(countdown) + " seconds remaining");
                server.handleClient();
            }
            delay(50);
        }

        countdown = 0;
        isEntryCooldown = false; // Re-enable IR triggers
        Serial.println("10-second delay finished, IR re-enabled");
        client.stop();
        return 0;
    } else {
        currentStatus = "Invalid Plate [No Entry]";
        Serial.println(currentStatus);
        client.stop();
        countdown = 0;
        return 2;
    }
}

void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    Serial.begin(115200);
    delay(100);
    Serial.println("Starting setup...");
    Serial.flush();

    // Initialize UART
    Serial.println("Initializing UART...");
    Serial1.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);
    Serial.println("UART initialized");
    Serial.flush();

    // Initialize pins
    Serial.println("Initializing pins...");
    pinMode(flashLight, OUTPUT);
    pinMode(inSensor, INPUT_PULLUP);
    digitalWrite(flashLight, LOW);
    Serial.println("Pins initialized");
    Serial.flush();

    // Connect to WiFi
    Serial.println("Connecting to WiFi...");
    WiFi.mode(WIFI_STA);
    Serial.print("Connecting to "); Serial.println(ssid);
    WiFi.begin(ssid, password);
    unsigned long wifiTimeout = millis() + 10000;
    while (WiFi.status() != WL_CONNECTED && millis() < wifiTimeout) {
        Serial.print(".");
        delay(500);
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nWiFi connection failed, restarting...");
        Serial.flush();
        delay(1000);
        ESP.restart();
    }
    Serial.println("\nWiFi connected");
    Serial.print("ESP32-CAM IP Address: "); Serial.println(WiFi.localIP());
    Serial.println("Access web server at: http://" + WiFi.localIP().toString() + "/");
    Serial.flush();

    // Initialize NTP
    Serial.println("Initializing NTP...");
    timeClient.begin();
    if (!timeClient.update()) {
        Serial.println("NTP update failed");
    } else {
        Serial.println("NTP initialized");
    }
    Serial.flush();

    // Start web server
    Serial.println("Starting web server...");
    server.on("/", handleRoot);
    server.on("/trigger", HTTP_POST, handleTrigger);
    server.on("/exit", HTTP_POST, handleExit);
    server.on("/clearHistory", handleClearHistory);
    server.begin();
    Serial.println("Web server started");
    Serial.flush();

    // Initialize camera
    Serial.println("Initializing camera...");
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 20;
    config.fb_count = 1;
    
    int cameraRetries = 3;
    esp_err_t err;
    while (cameraRetries--) {
        err = esp_camera_init(&config);
        if (err == ESP_OK) break;
        Serial.printf("Camera init failed with error 0x%x, retrying...\n", err);
        Serial.flush();
        delay(1000);
    }
    if (err != ESP_OK) {
        Serial.println("Camera init failed, restarting...");
        Serial.flush();
        delay(5000);
        ESP.restart();
    }
    Serial.println("Camera initialized successfully");
    Serial.flush();

    clearHistory();
    Serial.println("Setup complete");
    Serial.flush();
}

void loop() {
    unsigned long currentMillis = millis();

    // Handle WiFi reconnection
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi disconnected, attempting to reconnect...");
        Serial.flush();
        WiFi.reconnect();
        unsigned long wifiTimeout = millis() + 5000;
        while (WiFi.status() != WL_CONNECTED && millis() < wifiTimeout) {
            delay(500);
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.print("Reconnected, IP: "); Serial.println(WiFi.localIP());
        } else {
            Serial.println("Reconnect failed, restarting...");
            ESP.restart();
        }
        Serial.flush();
    }

    // Read UART data from Nano
    if (Serial1.available()) {
        char c = Serial1.read();
        if (c == '\n') {
            parseUARTData(uartBuffer);
            uartBuffer = "";
            lastUARTRead = currentMillis;
        } else {
            uartBuffer += c;
        }
    } else if (currentMillis - lastUARTRead > 5000 && uartBuffer.length() > 0) {
        Serial.println("UART timeout, clearing buffer: '" + uartBuffer + "'");
        Serial.flush();
        uartBuffer = "";
        lastUARTRead = currentMillis;
    }

    // Update time
    timeClient.update();
    currentTime = timeClient.getFormattedTime();

    // Clear exit message after timeout
    if (currentMillis - exitMessageTime > exitMessageTimeout && exitMessage.length() > 0) {
        exitMessage = "";
        exitMessageTime = 0;
    }

    // Handle web server requests
    static unsigned long lastServerHandle = 0;
    if (currentMillis - lastServerHandle >= 50) {
        server.handleClient();
        lastServerHandle = currentMillis;
    }

    // Entry sensor trigger
    if (digitalRead(inSensor) == LOW && currentMillis - lastEntryTrigger > debounceDelay && vehicleCount < availableSpaces && !isEntryCooldown) {
        lastEntryTrigger = currentMillis;
        Serial.println("Entry sensor triggered (GPIO 13) at " + String(currentMillis) + "ms");
        Serial.flush();
        handleTrigger();
    } else if (digitalRead(inSensor) == LOW && isEntryCooldown) {
        Serial.println("Entry sensor triggered but ignored (in cooldown) at " + String(currentMillis) + "ms");
        Serial.flush();
    }
}
