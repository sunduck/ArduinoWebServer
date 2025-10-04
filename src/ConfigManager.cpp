#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "ConfigManager.h"

Preferences preferences;

ConfigManager::ConfigManager() {
  // Defaults (same as reset, but without saving to flash)
  mode = "growing";
  lightStart = 23;
  lightEnd = 17;
  sensorSettleTime = 300;
  soilLogIntervalMin = 15;
  //wateringEnabled = true;

  setDefaultSchedules();
}

void ConfigManager::load() {
    if (!preferences.begin("garden", true)) {
        Serial.println("[Config] Failed to open NVS in read mode, using defaults");
        setDefaultSchedules();
        return;
    }

    mode = preferences.getString("mode", "growing");
    lightStart = preferences.getInt("lightStart", 23);
    lightEnd = preferences.getInt("lightEnd", 17);
    sensorSettleTime = preferences.getInt("snsTime", 300);
    soilLogIntervalMin = preferences.getInt("soilIntrvl", 15);

    wateringSchedules.clear();
    if (preferences.isKey("wSchdl")) {
        String schedulesJson = preferences.getString("wSchdl", "");
        if (schedulesJson.length() > 0) {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, schedulesJson);
            if (!err && doc.is<JsonArray>()) {
                for (JsonObject obj : doc.as<JsonArray>()) {
                    WateringSchedule ws;
                    ws.time = obj["time"].as<String>();
                    JsonArray arr = obj["durations"].as<JsonArray>();
                    for (int i = 0; i < 4; i++) {
                        ws.durations[i] = arr[i].as<int>();
                    }
                    wateringSchedules.push_back(ws);
                }
                Serial.printf("[Config] Loaded %d watering schedules from NVS\n", wateringSchedules.size());
            } else {
                Serial.println("[Config] Failed to parse wateringSchedules, using defaults");
                setDefaultSchedules();
            }
        } else {
            Serial.println("[Config] wateringSchedules key empty, using defaults");
            setDefaultSchedules();
        }
    } else {
        Serial.println("[Config] wateringSchedules key not found in NVS, using defaults");
        setDefaultSchedules();
    }

    preferences.end();
}

void ConfigManager::save() {
  if (!preferences.begin("garden", false)) {
      Serial.println("[Config] Failed to open NVS in write mode, cannot save");
      return;
  }

  preferences.putString("mode", mode);
  preferences.putInt("lightStart", lightStart);
  preferences.putInt("lightEnd", lightEnd);
  preferences.putInt("snsTime", sensorSettleTime);
  preferences.putInt("soilIntrvl", soilLogIntervalMin);

  // Serialize wateringSchedules as JSON
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto &ws : wateringSchedules) {
    JsonObject obj = arr.add<JsonObject>();
    obj["time"] = ws.time;
    JsonArray d = obj["durations"].to<JsonArray>();
    for (int v = 0; v < 4; v++) d.add(ws.durations[v]);
  }
  String json;
  serializeJson(doc, json);
  preferences.putString("wSchdl", json);

  preferences.end();
}

void ConfigManager::reset() {
  if (!preferences.begin("garden", false)) {
      Serial.println("[Config] Failed to open NVS in write mode, cannot reset");
      return;
  }
  preferences.clear();

  // Restore defaults
  mode = "growing";
  lightStart = 23;
  lightEnd = 17;
  sensorSettleTime = 300;
  soilLogIntervalMin = 15;
  //wateringEnabled = true;

  setDefaultSchedules();

  save();
  preferences.end();
}

void ConfigManager::setDefaultSchedules() {
    wateringSchedules.clear();
    wateringSchedules.push_back({"23:00", {60, 65, 68, 60}});
    wateringSchedules.push_back({"05:00", {30, 35, 30, 30}});
    wateringSchedules.push_back({"11:00", {30, 35, 30, 30}});
}