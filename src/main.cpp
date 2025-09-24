#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include "WiFiCredentials.h"
#include "ConfigManager.h"

// Relay pins (active LOW, HIGH = OFF, LOW = ON)
const int relayPins[8] = {18, 17, 16, 15, 7, 6, 5, 4};

// Soil sensor analog input pins
const int soilPins[4]  = {10, 9, 11, 3};

// Store last soil readings
int lastSoilReadings[4] = {0, 0, 0, 0};

AsyncWebServer server(80);
ConfigManager config;

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial0.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial0.print(".");
  }
  Serial0.println();
  Serial0.print("Connected! IP: ");
  Serial0.println(WiFi.localIP());
}

void setupNTP() {
  configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // MSK-3
  Serial0.println("NTP configured");
}

// Soil moisture task
void soilTask(void *pvParameters) {
  for (;;) {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    // Run only during light cycle
    if (timeinfo.tm_hour >= config.lightStart && timeinfo.tm_hour < config.lightEnd) {
      if (timeinfo.tm_min % 15 == 0) {
        // Power ON sensors
        for (int i = 0; i < 4; i++) {
          digitalWrite(relayPins[i], LOW); // active LOW → ON
        }
        delay(500); // stabilization

        // Read soil sensors with averaging
        for (int i = 0; i < 4; i++) {
          long sum = 0;
          const int samples = 5;
          for (int j = 0; j < samples; j++) {
            sum += analogRead(soilPins[i]);
            delay(50);
          }
          int value = sum / samples;
          lastSoilReadings[i] = value;
          Serial0.printf("Soil sensor %d (avg of %d): %d\n", i, samples, value);
        }

        // Power OFF sensors
        for (int i = 0; i < 4; i++) {
          digitalWrite(relayPins[i], HIGH); // OFF
        }

        vTaskDelay(60000 / portTICK_PERIOD_MS); // wait until next minute
      }
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void setupServer() {
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["wifi"] = WiFi.SSID();
    doc["ip"]   = WiFi.localIP().toString();
    doc["mode"] = config.mode;
    doc["lightStart"] = config.lightStart;
    doc["lightEnd"]   = config.lightEnd;

    // Watering times
    JsonArray arr = doc["wateringTimes"].to<JsonArray>();
    for (auto &t : config.wateringTimes) arr.add(t);

    // Soil readings
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

      if (doc["mode"].is<String>()) {
        config.mode = doc["mode"].as<String>();
      }
      if (doc["lightStart"].is<int>()) {
        config.lightStart = doc["lightStart"].as<int>();
      }
      if (doc["lightEnd"].is<int>()) {
        config.lightEnd = doc["lightEnd"].as<int>();
      }
      if (doc["wateringTimes"].is<JsonArray>()) {
        config.wateringTimes.clear();
        for (JsonVariant v : doc["wateringTimes"].as<JsonArray>()) {
          if (v.is<String>()) {
            config.wateringTimes.push_back(v.as<String>());
          }
        }
      }

      config.save();
      request->send(200, "application/json", "{\"status\":\"ok\"}");
    });

  server.on("/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
    config.reset();
    request->send(200, "application/json", "{\"status\":\"reset\"}");
  });

  server.begin();
}

void setup() {
  Serial0.begin(115200);
  setupWiFi();
  setupNTP();
  config.load();

  // Init all 8 relays OFF
  for (int i = 0; i < 8; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], HIGH); // default OFF
  }

  setupServer();

  // Start soil task
  xTaskCreatePinnedToCore(soilTask, "SoilTask", 4096, NULL, 1, NULL, 1);
}

void loop() {
  // Nothing here, tasks + server handle everything
}
