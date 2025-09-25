#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include "WiFiCredentials.h"
#include "ConfigManager.h"

// ===============================================================
// ESP32 Garden Controller
// Hardware: ESP32-S3-N16R8
// Features:
//   - 8x 5V relays (active LOW) → general control (soil sensors power, etc.)
//   - 4x 12V relays (active HIGH) → extra devices
//   - 4x Soil humidity sensors (analog inputs)
//   - WiFi + NTP required for operation
//   - REST API via AsyncWebServer
//   - ConfigManager stores config (mode, light cycle, watering times, settle time)
//   - Soil readings every 15 minutes during light cycle
//   - SoilLog history reset at each lightStart
// ===============================================================

// --- Pin assignments ---
const int relay5vPins[8] = {18, 17, 16, 15, 7, 6, 5, 4};
const int relay12vPins[4] = {47, 21, 20, 19};
const int soilPins[4]  = {10, 9, 11, 3};

// --- Globals ---
int lastSoilReadings[4] = {0, 0, 0, 0};

struct SoilLog {
  time_t timestamp;
  int values[4];
};

const int MAX_LOGS = 500;
SoilLog soilLogs[MAX_LOGS];
int logIndex = 0;
int logCount = 0;

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

// --- Soil log helpers ---
void addSoilLog() {
  SoilLog entry;
  entry.timestamp = time(nullptr);
  for (int i = 0; i < 4; i++) entry.values[i] = lastSoilReadings[i];

  soilLogs[logIndex] = entry;
  logIndex = (logIndex + 1) % MAX_LOGS;
  if (logCount < MAX_LOGS) logCount++;

  logDebug("Soil log added");
}

void resetSoilLogs() {
  logIndex = 0;
  logCount = 0;
  logDebug("Soil log reset at lightStart");
}

// --- WiFi + NTP ---
void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial0.print("Connecting to WiFi");
  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial0.print(".");
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
    digitalWrite(relay5vPins[i], LOW); // ON
    delay(config.sensorSettleTime);

    long sum = 0;
    for (int j = 0; j < samples; j++) {
      sum += analogRead(soilPins[i]);
      delay(50);
    }
    int value = sum / samples;
    lastSoilReadings[i] = value;
    logDebug("Soil sensor " + String(i) + ": " + String(value));

    digitalWrite(relay5vPins[i], HIGH); // OFF
  }
}

// --- Soil task ---
void soilTask(void *pvParameters) {
  int lastHour = -1;

  for (;;) {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    // Reset logs at start of light cycle
    if (timeinfo.tm_hour == config.lightStart && timeinfo.tm_hour != lastHour) {
      resetSoilLogs();
    }
    lastHour = timeinfo.tm_hour;

    // Check if inside light cycle
    bool inLightCycle;
    if (config.lightStart < config.lightEnd) {
      inLightCycle = (timeinfo.tm_hour >= config.lightStart && timeinfo.tm_hour < config.lightEnd);
    } else {
      inLightCycle = (timeinfo.tm_hour >= config.lightStart || timeinfo.tm_hour < config.lightEnd);
    }

    // Every 15 minutes
    if (inLightCycle && (timeinfo.tm_min % 15 == 0)) {
      readSoilSensors();
      addSoilLog();
      vTaskDelay(60000 / portTICK_PERIOD_MS);
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

  // Register history BEFORE sensors
  server.on("/sensors/history", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    int idx = logIndex;
    for (int i = 0; i < logCount; i++) {
      idx = (idx - 1 + MAX_LOGS) % MAX_LOGS;
      SoilLog &entry = soilLogs[idx];

      JsonObject obj = arr.add<JsonObject>();

      // Human-readable timestamp
      char buf[25];
      struct tm timeinfo;
      localtime_r(&entry.timestamp, &timeinfo);
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
      obj["timestamp"] = buf;

      JsonArray vals = obj["values"].to<JsonArray>();
      for (int j = 0; j < 4; j++) vals.add(entry.values[j]);
    }

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  server.on("/sensors", HTTP_GET, [](AsyncWebServerRequest *request) {
    readSoilSensors();
    addSoilLog();

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

  setupWiFi();
  setupNTP();
  config.load();
  setupServer();

  xTaskCreatePinnedToCore(soilTask, "SoilTask", 4096, NULL, 1, NULL, 1);
}

void loop() {}
