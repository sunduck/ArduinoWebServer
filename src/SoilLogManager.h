#pragma once
#include <Arduino.h>
#include <time.h>

// Definition of SoilLog
struct SoilLog {
  time_t timestamp;
  int values[4];
};

// Globals
extern SoilLog soilLogs[];
extern int logIndex;
extern int logCount;
extern const int MAX_LOGS;

// Functions
void addSoilLog(int lastSoilReadings[4]);
void resetSoilLogs();
