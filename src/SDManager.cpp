#include "SDManager.h"
#include "SoilLogManager.h"
#include "ConfigManager.h"
#include <ArduinoJson.h>

SPIClass spi(HSPI);
bool sdAvailable = false;

extern String getTimestamp(); // from main
extern void logDebug(const String &msg); // from main

void setupSD() {
  spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, spi, 80000000)) {
    Serial0.println("[WARN] SD not available, running without it");
    sdAvailable = false;
    return;
  }
  sdAvailable = true;
  Serial0.println("[DEBUG] SD mounted successfully");
}

void dumpSoilLogsToSD() {
  if (!sdAvailable) {
    Serial0.println("[WARN] SD not available, skipping dump");
    return;
  }
  if (logCount == 0) {
    Serial0.println("[DEBUG] No soil logs to dump");
    return;
  }

  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  char filename[32];
  strftime(filename, sizeof(filename), "/soil_%Y-%m-%d.json", &timeinfo);

  File file = SD.open(filename, FILE_WRITE);
  if (!file) {
    Serial0.println("[ERROR] Failed to open log file for writing");
    return;
  }

  JsonDocument doc;
  doc["date"] = getTimestamp().substring(0, 10);
  JsonArray arr = doc["readings"].to<JsonArray>();

  int idx = logIndex;
  for (int i = 0; i < logCount; i++) {
    idx = (idx - 1 + MAX_LOGS) % MAX_LOGS;
    SoilLog &entry = soilLogs[idx];

    JsonObject obj = arr.add<JsonObject>();
    char buf[25];
    struct tm entryTime;
    localtime_r(&entry.timestamp, &entryTime);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &entryTime);
    obj["timestamp"] = buf;

    JsonArray vals = obj["values"].to<JsonArray>();
    for (int j = 0; j < 4; j++) vals.add(entry.values[j]);
  }

  serializeJsonPretty(doc, file);
  file.close();
  Serial0.printf("[DEBUG] Soil logs dumped to %s\n", filename);
}
