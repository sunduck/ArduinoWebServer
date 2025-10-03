#include "ServerManager.h"
#include "ConfigManager.h"
#include "SoilLogManager.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <time.h>

extern int lastSoilReadings[4];
extern ConfigManager config;
extern volatile bool pumpActive;

// forward declarations from main.cpp
void readSoilSensors();
void waterValve(int id, int seconds);

void setupServer() {
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["wifi"] = WiFi.SSID();
    doc["ip"]   = WiFi.localIP().toString();
    doc["mode"] = config.mode;
    doc["lightStart"] = config.lightStart;
    doc["lightEnd"]   = config.lightEnd;
    doc["sensorSettleTime"] = config.sensorSettleTime;
    doc["soilLogIntervalMin"] = config.soilLogIntervalMin;

    JsonArray arr = doc["wateringTimes"].to<JsonArray>();
    for (auto &t : config.wateringTimes) arr.add(t);

    JsonArray soil = doc["soilReadings"].to<JsonArray>();
    for (int i = 0; i < 4; i++) soil.add(lastSoilReadings[i]);

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

  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["mode"] = config.mode;
    doc["lightStart"] = config.lightStart;
    doc["lightEnd"] = config.lightEnd;
    doc["sensorSettleTime"] = config.sensorSettleTime;
    doc["soilLogIntervalMin"] = config.soilLogIntervalMin;

    JsonArray arr = doc["wateringTimes"].to<JsonArray>();
    for (auto &t : config.wateringTimes) arr.add(t);

    doc["save"] = false;

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

      if (doc["mode"].is<const char*>()) config.mode = String(doc["mode"].as<const char*>());
      if (doc["lightStart"].is<int>()) config.lightStart = doc["lightStart"].as<int>();
      if (doc["lightEnd"].is<int>()) config.lightEnd = doc["lightEnd"].as<int>();
      if (doc["sensorSettleTime"].is<int>()) config.sensorSettleTime = doc["sensorSettleTime"].as<int>();
      if (doc["soilLogIntervalMin"].is<int>()) config.soilLogIntervalMin = doc["soilLogIntervalMin"].as<int>();

      if (doc["wateringTimes"].is<JsonArray>()) {
        config.wateringTimes.clear();
        for (JsonVariant v : doc["wateringTimes"].as<JsonArray>()) {
          if (v.is<const char*>()) config.wateringTimes.push_back(String(v.as<const char*>()));
        }
      }

      bool persist = false;
      if (doc["save"].is<bool>() && doc["save"].as<bool>() == true) {
        persist = true;
      }

      if (persist) {
        config.save();
        request->send(200, "application/json", "{\"status\":\"saved\"}");
      } else {
        request->send(200, "application/json", "{\"status\":\"applied (RAM only)\"}");
      }
    });

  server.on("/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
    config.reset();
    request->send(200, "application/json", "{\"status\":\"reset\"}");
  });

  server.on("/logs", HTTP_GET, [](AsyncWebServerRequest *request) {
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

      JsonArray wateringArr = obj["watering"].to<JsonArray>();
      for (int j = 0; j < 4; j++) {
        if (entry.watering[j]) {
          JsonObject w = wateringArr.add<JsonObject>();
          w["valve"] = j;
          w["time"]  = entry.wateringTime[j];
        }
      }
    }

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
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
}
