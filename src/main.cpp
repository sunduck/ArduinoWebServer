#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include "WiFiCredentials.h"
#include "ConfigManager.h"
#include "SDManager.h"
#include "SoilLogManager.h"

// ===============================================================
// ESP32 Garden Controller
// Hardware: ESP32-S3-N16R8
// ===============================================================

// --- Relay & sensor pins ---
const int relay5vPins[8] = {18, 17, 16, 15, 7, 6, 5, 4};
const int relay12vPins[4] = {47, 21, 20, 19};
const int soilPins[4]     = {10, 9, 11, 3};

// --- Globals ---
int lastSoilReadings[4] = {0, 0, 0, 0};
AsyncWebServer server(80);
ConfigManager config;
volatile bool pumpActive = false;  // guard: only one watering at a time

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
  configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");

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

    if (timeinfo.tm_hour == config.lightStart && timeinfo.tm_hour != lastHour) {
      dumpSoilLogsToSD();
      resetSoilLogs();
    }
    lastHour = timeinfo.tm_hour;

    int startHour = (config.lightStart - 1 + 24) % 24; // start one hour earlier
    int endHour   = config.lightEnd;

    bool inLightCycle;
    if (startHour < endHour) {
      inLightCycle = (timeinfo.tm_hour >= startHour && timeinfo.tm_hour < endHour);
    } else {
      inLightCycle = (timeinfo.tm_hour >= startHour || timeinfo.tm_hour < endHour);
    }

    if (inLightCycle && (timeinfo.tm_min % 15 == 0)) {
      readSoilSensors();
      addSoilLog(lastSoilReadings);
      vTaskDelay(60000 / portTICK_PERIOD_MS);
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// --- Watering control ---
void waterValve(int id, int seconds) {
  if (id < 0 || id >= 4) {
    logDebug("Invalid valve ID: " + String(id));
    return;
  }

  if (pumpActive) {
    logDebug("Pump already active, rejecting watering request");
    return;
  }

  // Clamp duration
  if (seconds < 1) seconds = 1;
  if (seconds > 300) seconds = 300;

  logDebug("Watering valve " + String(id) + " for " + String(seconds) + "s");

  // Pump ON (relay5vPins[7] = pin 4, active LOW)
  digitalWrite(relay5vPins[7], LOW);
  // Valve ON (12V relay, active HIGH)
  digitalWrite(relay12vPins[id], HIGH);

  pumpActive = true;

  struct ValveArgs { int id; int seconds; };
  ValveArgs *args = new ValveArgs{id, seconds};

  xTaskCreatePinnedToCore(
    [](void *param) {
      ValveArgs *a = (ValveArgs*)param;
      vTaskDelay(a->seconds * 1000 / portTICK_PERIOD_MS);

      // Turn valve OFF
      digitalWrite(relay12vPins[a->id], LOW);
      // Turn pump OFF
      digitalWrite(relay5vPins[7], HIGH);

      pumpActive = false;

      logDebug("Valve " + String(a->id) + " OFF, pump OFF after watering");

      delete a;
      vTaskDelete(NULL);
    },
    "ValveTimer",
    2048,
    args,
    1,
    NULL,
    1
  );
}

// --- Web server ---
void setupServer() {
  // Status
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

    // Add timestamp of last reading
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char buf[25];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    doc["lastReadingTimestamp"] = buf;

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // Config update
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

  // Reset config
  server.on("/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
    config.reset();
    request->send(200, "application/json", "{\"status\":\"reset\"}");
  });

  // Sensors history
  server.on("/sensors/history", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    int idx = logIndex;
    for (int i = 0; i < logCount; i++) {
      idx = (idx - 1 + MAX_LOGS) % MAX_LOGS;
      SoilLog &entry = soilLogs[idx];

      JsonObject obj = arr.add<JsonObject>();

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

  // Sensors immediate read (no logging)
  server.on("/sensors", HTTP_GET, [](AsyncWebServerRequest *request) {
    readSoilSensors();   // updates lastSoilReadings only

    JsonDocument doc;
    JsonArray soil = doc["soilReadings"].to<JsonArray>();
    for (int i = 0; i < 4; i++) soil.add(lastSoilReadings[i]);

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // Watering endpoint
  server.on("/watering", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("id")) {
      request->send(400, "application/json", "{\"error\":\"Missing valve ID\"}");
      return;
    }

    int id = request->getParam("id")->value().toInt();
    if (id < 0 || id > 3) {
      request->send(400, "application/json", "{\"error\":\"Invalid valve ID (0-3)\"}");
      return;
    }

    if (pumpActive) {
      request->send(409, "application/json", "{\"error\":\"Pump already active\"}");
      return;
    }

    int seconds = 10;
    if (request->hasParam("time")) {
      seconds = request->getParam("time")->value().toInt();
    }
    if (seconds < 1 || seconds > 300) {
      request->send(400, "application/json", "{\"error\":\"Invalid time (1-300)\"}");
      return;
    }

    waterValve(id, seconds);

    JsonDocument doc;
    doc["valve"] = id;
    doc["time"] = seconds;
    doc["status"] = "started";
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
  setupSD();
  setupServer();

  xTaskCreatePinnedToCore(soilTask, "SoilTask", 4096, NULL, 1, NULL, 1);
}

void loop() {}
