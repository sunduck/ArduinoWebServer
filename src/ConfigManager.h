#pragma once

#include <vector>       // make sure STL vector is seen first
#include <Arduino.h>    // defines String and other Arduino types
#include <Preferences.h>
#pragma once
#include <Arduino.h>
#include <vector>
#include <array>

struct WateringSchedule {
    String time;                 // "HH:MM"
    std::array<int, 4> durations; // per-valve durations
};

class ConfigManager {
public:
    ConfigManager();

    void load();
    void save();
    void reset();
    void setDefaultSchedules();

    // --- Configurable values ---
    String mode;
    int lightStart;
    int lightEnd;
    int sensorSettleTime;
    int soilLogIntervalMin;

    std::vector<WateringSchedule> wateringSchedules; // ✅ new
};