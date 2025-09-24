#include "ConfigManager.h"

Preferences preferences;

void ConfigManager::load() {
  if (!preferences.begin("garden", true)) return;
  mode = preferences.getString("mode", "growing");
  lightStart = preferences.getInt("lightStart", 23);
  lightEnd = preferences.getInt("lightEnd", 17);

  wateringTimes.clear();
  for (int i = 0; i < 3; i++) {
    String key = "wt" + String(i);
    String t = preferences.getString(key.c_str(), "");
    if (t.length() > 0) wateringTimes.push_back(t);
  }
  preferences.end();
}

void ConfigManager::save() {
  if (!preferences.begin("garden", false)) return;
  preferences.putString("mode", mode);
  preferences.putInt("lightStart", lightStart);
  preferences.putInt("lightEnd", lightEnd);

  for (size_t i = 0; i < wateringTimes.size(); i++) {
    String key = "wt" + String(i);
    preferences.putString(key.c_str(), wateringTimes[i]);
  }
  preferences.end();
}

void ConfigManager::reset() {
  if (!preferences.begin("garden", false)) return;
  preferences.clear();
  preferences.end();
}
