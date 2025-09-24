#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <vector>

class ConfigManager {
public:
  String mode;
  int lightStart;
  int lightEnd;
  std::vector<String> wateringTimes;

  void load();
  void save();
  void reset();
};