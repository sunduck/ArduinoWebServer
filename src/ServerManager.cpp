#include "ServerManager.h"
#include "ConfigManager.h"
#include "LogManager.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <time.h>

extern uint16_t soilReadingsLast[4];
extern uint16_t soilReadingsMax[4];
extern uint16_t soilReadingsMin[4];
extern ConfigManager config;
extern LogManager logManager;
extern volatile bool pumpActive;

// forward declarations from main.cpp
void readSoilSensors();
void wateringCycle(int duration0, int duration1, int duration2, int duration3);

void setupServer()
{
  // status endpoint - returns current status as JSON
  // example response:
  /*
  {
    "wifi": "MySSID",
    "ip": "192.168.1.125",
    "mode": "growing",
    "lightStart": 23,
    "lightEnd": 17,
    "sensorSettleTime": 300,
    "soilLogIntervalMin": 15,
    "soilHumidityLast": [
        353,
        322,
        297,
        339
    ],
    "soilHumidityMin": [
        353,
        322,
        297,
        339
    ],
    "soilHumidityMax": [
        353,
        322,
        297,
        339
    ],
    "lastReadingTimestamp": "2025-10-05 16:33:41",
    "uptime": "1d 17h 1m 37s",
    "lastResetReason": "1",
    "pumpActive": false,
    "freeHeap": 185388,
    "flashChipSize": 16777216,
    "sketchSize": 895936,
    "freeSketchSpace": 6553600
}*/
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    JsonDocument doc;
    doc["wifi"] = WiFi.SSID();
    doc["ip"]   = WiFi.localIP().toString();
    doc["mode"] = config.mode;
    doc["lightStart"] = config.lightStart;
    doc["lightEnd"]   = config.lightEnd;
    doc["sensorSettleTime"] = config.sensorSettleTime;
    doc["soilLogIntervalMin"] = config.soilLogIntervalMin;

    JsonArray soilLast = doc["soilHumidityLast"].to<JsonArray>();
    for (int i = 0; i < 4; i++) soilLast.add(soilReadingsLast[i]);
    JsonArray soilMax = doc["soilHumidityMax"].to<JsonArray>();
    for (int i = 0; i < 4; i++) soilMax.add(soilReadingsMax[i]);
    JsonArray soilMin = doc["soilHumidityMin"].to<JsonArray>();
    for (int i = 0; i < 4; i++) soilMin.add(soilReadingsMin[i]);

    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char buf[25];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    doc["lastReadingTimestamp"] = buf;
    int64_t us = esp_timer_get_time();   // microseconds since boot
    uint64_t s = us / 1000000ULL;        // convert to seconds

    uint32_t days    = s / 86400;
    uint32_t hours   = (s % 86400) / 3600;
    uint32_t minutes = (s % 3600) / 60;
    uint32_t seconds = s % 60;
    doc["uptime"] = String(days) + "d " + String(hours) + "h " + String(minutes) + "m " + String(seconds) + "s";
    doc["lastResetReason"] = String(esp_reset_reason());
    doc["pumpActive"] = pumpActive;
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["flashChipSize"] = ESP.getFlashChipSize();
    doc["sketchSize"] = ESP.getSketchSize();
    doc["freeSketchSpace"] = ESP.getFreeSketchSpace();

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json); });

  // config endpoint - GET returns current config as JSON
  // POST with JSON body to update config (and optionally save to flash)
  // example GET response:
  /*
  {
  "mode": "growing",
  "lightStart": 23,
  "lightEnd": 17,
  "sensorSettleTime": 300,
  "soilLogIntervalMin": 15,
  "wateringSchedules": [
      {
          "time": "23:00",
          "durations": [
              45,
              45,
              45,
              45
          ]
      },
      {
          "time": "05:00",
          "durations": [
              30,
              30,
              30,
              30
          ]
      },
      {
          "time": "11:00",
          "durations": [
              30,
              30,
              30,
              30
          ]
      }
  ]
}
*/
  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request)
            {
        JsonDocument outDoc;
        outDoc["mode"] = config.mode;
        outDoc["lightStart"] = config.lightStart;
        outDoc["lightEnd"] = config.lightEnd;
        outDoc["sensorSettleTime"] = config.sensorSettleTime;
        outDoc["soilLogIntervalMin"] = config.soilLogIntervalMin;
        outDoc["soilSensorCounter"] = config.soilSensorCounter;

        JsonArray arr = outDoc["wateringSchedules"].to<JsonArray>();
        for (auto &ws : config.wateringSchedules) {
            JsonObject obj = arr.add<JsonObject>();
            obj["time"] = ws.time;
            JsonArray d = obj["durations"].to<JsonArray>();
            for (int v = 0; v < 4; v++) d.add(ws.durations[v]);
        }

        String json;
        serializeJson(outDoc, json);
        request->send(200, "application/json", json); });

  server.on("/config", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
            {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err) {
            request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            return;
        }

        // --- Apply basic fields ---
        if (doc["mode"].is<const char*>()) config.mode = String(doc["mode"].as<const char*>());
        if (doc["lightStart"].is<int>()) config.lightStart = doc["lightStart"].as<int>();
        if (doc["lightEnd"].is<int>()) config.lightEnd = doc["lightEnd"].as<int>();
        if (doc["sensorSettleTime"].is<int>()) config.sensorSettleTime = doc["sensorSettleTime"].as<int>();
        if (doc["soilLogIntervalMin"].is<int>()) config.soilLogIntervalMin = doc["soilLogIntervalMin"].as<int>();
        if (doc["soilSensorCounter"].is<int>()) config.soilSensorCounter = doc["soilSensorCounter"].as<int>();

        // --- Validate and apply watering schedules ---
        if (doc["wateringSchedules"].is<JsonArray>()) {
            std::vector<WateringSchedule> newSchedules;

            for (JsonObject obj : doc["wateringSchedules"].as<JsonArray>())
            {
              if (!obj["time"].is<const char *>() || !obj["durations"].is<JsonArray>())
              {
                request->send(400, "application/json", "{\"error\":\"wateringSchedules must contain time and durations\"}");
                return;
              }

              String t = obj["time"].as<String>();
              if (t.length() != 5 || t.charAt(2) != ':')
              {
                request->send(400, "application/json", "{\"error\":\"Invalid time format, must be HH:MM\"}");
                return;
              }

              JsonArray arr = obj["durations"].as<JsonArray>();
              if (arr.size() != 4)
              {
                request->send(400, "application/json", "{\"error\":\"Each schedule must have exactly 4 durations\"}");
                return;
              }

              WateringSchedule ws;
              ws.time = t;
              for (int i = 0; i < 4; i++)
              {
                int d = arr[i].as<int>();
                if (d < 0 || d > 600)
                { // limit 0–600 sec for safety
                  request->send(400, "application/json", "{\"error\":\"Duration out of range (0–600)\"}");
                  return;
                }
                ws.durations[i] = d;
              }
              newSchedules.push_back(ws);
            }

            // Replace only if all schedules valid
            config.wateringSchedules = newSchedules;
        }

        // Save if requested
        if (doc["save"].is<bool>() && doc["save"].as<bool>()) {
            config.save();
        }

        // --- Respond with full updated config (same as GET) ---
        JsonDocument outDoc;
        outDoc["mode"] = config.mode;
        outDoc["lightStart"] = config.lightStart;
        outDoc["lightEnd"] = config.lightEnd;
        outDoc["sensorSettleTime"] = config.sensorSettleTime;
        outDoc["soilLogIntervalMin"] = config.soilLogIntervalMin;
        outDoc["soilSensorCounter"] = config.soilSensorCounter;

        JsonArray arr = outDoc["wateringSchedules"].to<JsonArray>();
        for (auto &ws : config.wateringSchedules) {
            JsonObject obj = arr.add<JsonObject>();
            obj["time"] = ws.time;
            JsonArray d = obj["durations"].to<JsonArray>();
            for (int v = 0; v < 4; v++) d.add(ws.durations[v]);
        }

        String json;
        serializeJson(outDoc, json);
        request->send(200, "application/json", json); });
  // reset endpoint - resets config to defaults
  // require to recover from BAD or create NEW config on config structure change
  server.on("/reset", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    config.reset();
    request->send(200, "application/json", "{\"status\":\"reset\"}"); });

  server.on("/logs", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
  int count = logManager.getEventCount();
    for (int i = 0; i < count; i++) {
  Event event = logManager.getEvent(i);
      JsonObject obj = arr.add<JsonObject>();
      // Format timestamp as 'YYYY-MM-DD HH:MM:SS'
      char buf[25];
      time_t t = event.timestamp;
      struct tm timeinfo;
      localtime_r(&t, &timeinfo);
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
      obj["timestamp"] = buf;
      obj["timestamp"] = buf;
      obj["eventType"] = logManager.getEventTypeName(event.eventType);
      obj["value"] = event.value;
    }
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json); });

  // sensors endpoint - mannualy reads soil sensors and returns current readings as JSON
  server.on("/sensors", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    readSoilSensors();
    JsonDocument doc;
    JsonArray soil = doc["soilReadingsLast"].to<JsonArray>();
    for (int i = 0; i < 4; i++) soil.add(soilReadingsLast[i]);
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json); });

  // watering endpoint - starts watering cycle with optional durations for each valve
  // example: /watering?duration0=30&duration1=45&duration2=0&duration3=15
  // durations in seconds, if not specified valve will be skipped
  // if pump already active, returns 409 error
  server.on("/watering", HTTP_POST, [](AsyncWebServerRequest *request)
            {

    if (pumpActive) {
      request->send(409, "application/json", "{\"error\":\"Pump already active\"}");
      return;
    }

    int duration0 = 0;
    if (request->hasParam("duration0")) {
      duration0 = request->getParam("duration0")->value().toInt();
    }
    int duration1 = 0;
    if (request->hasParam("duration1")) {
      duration1 = request->getParam("duration1")->value().toInt();
    }
    int duration2 = 0;
    if (request->hasParam("duration2")) {
      duration2 = request->getParam("duration2")->value().toInt();
    }
    int duration3 = 0;
    if (request->hasParam("duration3")) {
      duration3 = request->getParam("duration3")->value().toInt();
    }

    wateringCycle(duration0, duration1, duration2, duration3);
    JsonDocument doc;
    doc["duration0"] = duration0;
    doc["duration1"] = duration1;
    doc["duration2"] = duration2;
    doc["duration3"] = duration3;
    doc["status"] = "started";
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json); });

  server.begin();
}
