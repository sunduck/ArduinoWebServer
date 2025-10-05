#pragma once
#include <Arduino.h>

#define MAX_LOGS 128

struct SoilLog {
  time_t timestamp;
  int values[4];             // soil sensor values
  bool watering[4];          // true if watering happened on this valve
  int wateringTime[4];       // duration (s) for each valve, 0 if none
};

extern SoilLog soilLogs[MAX_LOGS];
extern int logIndex;
extern int logCount;

void addSoilLog(int values[4], int wateringValve = -1, int wateringSeconds = 0);
void resetSoilLogs();