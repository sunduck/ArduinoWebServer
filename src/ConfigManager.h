#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <vector>

class ConfigManager {
public:
  String mode;
  int lightStart;
  int lightEnd;
  int sensorSettleTime;              // NEW: delay for soil sensor stabilization (ms)
  std::vector<String> wateringTimes;

  void load();
  void save();
  void reset();
};
