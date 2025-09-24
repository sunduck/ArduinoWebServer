#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include "WiFiCredentials.h"
#include "ConfigManager.h"

// 5V Relay pins (active LOW, HIGH = OFF, LOW = ON)
const int relay5vPins[8] = {18, 17, 16, 15, 7, 6, 5, 4};

// 12V Relay pins (active HIGH, LOW = OFF, HIGH = ON)
const int relay12vPins[4] = {47, 21, 20, 19};

// Soil sensor analog input pins
const int soilPins[4]  = {10, 9, 11, 3};

// Store last soil readings
int lastSoilReadings[4] = {0, 0, 0, 0};

AsyncWebServer server(80);
ConfigManager config;

// --- Logging helpers ---
String getTimestamp() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

void logDebug(const String &msg) {
  Serial0.print("[");
  Serial0.print(getTimestamp());
  Serial0.print("] ");
  Serial0.println(msg);
}

// --- WiFi + NTP setup ---
void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial0.print("Connecting to WiFi");
  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial0.print(".");

    // If > 30s without WiFi, reboot
    if (millis() - startAttempt > 30000) {
      Serial0.println("\n[ERROR] WiFi connection failed, rebooting...");
      ESP.restart();
    }
  }

  Serial0.println();
  Serial0.print("Connected! IP: ");
  Serial0.println(WiFi.localIP());
}

void setupNTP() {
  configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // MSK-3

  struct tm timeinfo;
  Serial0.print("Syncing time via NTP");
  unsigned long startAttempt = millis();

  while (!getLocalTime(&timeinfo)) {
    delay(500);
    Serial0.print(".");

    // If > 30s without time sync, reboot
    if (millis() - startAttempt > 30000) {
      Serial0.println("\n[ERROR] NTP sync failed, rebooting...");
      ESP.restart();
    }
  }

  Serial0.println();
  Serial0.println("NTP time synced successfully");
  logDebug("NTP sync successful, timestamped logging enabled");
}

// --- Soil sensors ---
void readSoilSensors() {
  const int samples = 5;

  for (int i = 0; i < 4; i++) {
    // Power ON current sensor
    digitalWrite(relay5vPins[i], LOW); // active LOW → ON
    delay(config.sensorSettleTime);    // configurable stabilization

    // Read with averaging
    long sum = 0;
    for (int j = 0; j < samples; j++) {
      sum += analogRead(soilPins[i]);
      delay(50);
    }
    int value = sum / samples;
    lastSoilReadings[i] = value;
    logDebug("Soil sensor " + String(i) + ": " + String(value));

    // Power OFF current sensor
    digitalWrite(relay5vPins[i], HIGH); // OFF
  }
}

void soilTask(void *pvParameters) {
  for (;;) {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_hour >= config.lightStart && timeinfo.tm_hour < config.lightEnd) {
      if (timeinfo.tm_min % 15 == 0) {
        readSoilSensors();
        vTaskDelay(60000 / portTICK_PERIOD_MS);
      }
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// --- Web server ---
void setupServer() {
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["wifi"] = WiFi.SSID();
    doc["ip"]   = WiFi.localIP().toString();
    doc["mode"] = config.mode;
    doc["lightStart"] = config.lightStart;
    doc["lightEnd"]   = config.lightEnd;
    doc["sensorSettleTime"] = config.sensorSettleTime;

    JsonArray arr = doc["wateringTimes"].to<JsonArray>();
    for (auto &t : config.wateringTimes) arr.add(t);

    JsonArray soil = doc["soilReadings"].to<JsonArray>();
    for (int i = 0; i < 4; i++) soil.add(lastSoilReadings[i]);

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  server.on("/config", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, data, len);
      if (err) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      if (doc["mode"].is<String>()) config.mode = doc["mode"].as<String>();
      if (doc["lightStart"].is<int>()) config.lightStart = doc["lightStart"].as<int>();
      if (doc["lightEnd"].is<int>()) config.lightEnd = doc["lightEnd"].as<int>();
      if (doc["sensorSettleTime"].is<int>()) config.sensorSettleTime = doc["sensorSettleTime"].as<int>();

      if (doc["wateringTimes"].is<JsonArray>()) {
        config.wateringTimes.clear();
        for (JsonVariant v : doc["wateringTimes"].as<JsonArray>()) {
          if (v.is<String>()) config.wateringTimes.push_back(v.as<String>());
        }
      }

      config.save();
      request->send(200, "application/json", "{\"status\":\"ok\"}");
    });

  server.on("/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
    config.reset();
    request->send(200, "application/json", "{\"status\":\"reset\"}");
  });

  server.on("/sensors", HTTP_GET, [](AsyncWebServerRequest *request) {
    readSoilSensors();
    JsonDocument doc;
    JsonArray soil = doc["soilReadings"].to<JsonArray>();
    for (int i = 0; i < 4; i++) soil.add(lastSoilReadings[i]);

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  server.begin();
  logDebug("Web server started");
}

// --- Setup + loop ---
void setup() {
  Serial0.begin(115200);

  // Pin initialization first
  for (int i = 0; i < 8; i++) {
    pinMode(relay5vPins[i], OUTPUT);
    digitalWrite(relay5vPins[i], HIGH);
  }
  Serial0.println("[DEBUG] 5V relays initialized (default OFF, active LOW)");

  for (int i = 0; i < 4; i++) {
    pinMode(relay12vPins[i], OUTPUT);
    digitalWrite(relay12vPins[i], LOW);
  }
  Serial0.println("[DEBUG] 12V relays initialized (default OFF, active HIGH)");

  for (int i = 0; i < 4; i++) {
    pinMode(soilPins[i], INPUT);
  }
  Serial0.println("[DEBUG] Soil sensor pins set as INPUT");

  // System setup
  setupWiFi();
  setupNTP();
  config.load();
  setupServer();

  // Start soil task
  xTaskCreatePinnedToCore(soilTask, "SoilTask", 4096, NULL, 1, NULL, 1);
}

void loop() {
  // Nothing here
}
