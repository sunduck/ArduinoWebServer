#pragma once
#include <vector>       // make sure STL vector is seen first
#include <Arduino.h>    // defines String and other Arduino types
#include <Preferences.h>

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
    int soilSensorCounter; // amuont of readings to average per sensor

    std::vector<WateringSchedule> wateringSchedules;
};