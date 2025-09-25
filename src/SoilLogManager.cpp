#include "SoilLogManager.h"

const int MAX_LOGS = 500;
SoilLog soilLogs[MAX_LOGS];
int logIndex = 0;
int logCount = 0;

extern void logDebug(const String &msg); // from main.cpp

void addSoilLog(int lastSoilReadings[4]) {
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
