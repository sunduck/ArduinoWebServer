#include "SoilLogManager.h"

SoilLog soilLogs[MAX_LOGS];
int logIndex = 0;
int logCount = 0;

void addSoilLog(int values[4], int wateringValve, int wateringTime) {
  SoilLog &entry = soilLogs[logIndex];
  entry.timestamp = time(nullptr);

  for (int i = 0; i < 4; i++) {
    entry.values[i] = values[i];
    entry.watering[i] = false;
    entry.wateringTime[i] = 0;
  }

  if (wateringValve >= 0 && wateringValve < 4) {
    entry.watering[wateringValve] = true;
    entry.wateringTime[wateringValve] = wateringTime;
  }

  logIndex = (logIndex + 1) % MAX_LOGS;
  if (logCount < MAX_LOGS) logCount++;
}

void resetSoilLogs() {
  logIndex = 0;
  logCount = 0;

  for (int i = 0; i < MAX_LOGS; i++) {
    soilLogs[i].timestamp = 0;
    for (int j = 0; j < 4; j++) {
      soilLogs[i].values[j] = 0;
      soilLogs[i].watering[j] = false;
      soilLogs[i].wateringTime[j] = 0;
    }
  }
}
