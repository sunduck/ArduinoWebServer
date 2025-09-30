#pragma once

#include <vector>       // make sure STL vector is seen first
#include <Arduino.h>    // defines String and other Arduino types
#include <Preferences.h>
class ConfigManager {
public:
  String mode;
  int lightStart;
  int lightEnd;
  int sensorSettleTime;      // delay for soil sensor stabilization (ms)
  int soilLogIntervalMin;    // NEW: soil logging interval in minutes
  std::vector<String> wateringTimes;

  void load();
  void save();
  void reset();
};