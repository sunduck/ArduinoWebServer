#pragma once
#include <Arduino.h>

#define  MAX_LOGS 512

// Log uses ring buffer, overwriting oldest events when full

typedef enum
{
  EVENT_UNKNOWN,         //!< Event reason can not be determined
  EVENT_SOIL_READINGS_0, //!< Soil humidity reading on sensor 0
  EVENT_SOIL_READINGS_1, //!< Soil humidity reading on sensor 1
  EVENT_SOIL_READINGS_2, //!< Soil humidity reading on sensor 2
  EVENT_SOIL_READINGS_3, //!< Soil humidity reading on sensor 3
  EVENT_WATERING_0,      //!< Watering event on valve 0
  EVENT_WATERING_1,      //!< Watering event on valve 1
  EVENT_WATERING_2,      //!< Watering event on valve 2
  EVENT_WATERING_3       //!< Watering event on valve 3
} event_type_t;

struct Event
{
  time_t timestamp;
  event_type_t eventType; //!< Event type
  uint16_t value;          //!< Depending on event type: soil humidity (1-4096) or watering duration (s)
};

class LogManager {
public:
    LogManager();

    void addSoilEvent(uint8_t sensorId, int value);
    void addWaterEvent(uint8_t valveId, int durationSec);
    void clear();
    String getEventTypeName(event_type_t type);
    int getEventCount() const;
    Event getEvent(int index) const;

private:
    void pushEvent(const Event &event);
    SemaphoreHandle_t mutex;
    Event log[MAX_LOGS];
    size_t head;     // next write position
    size_t count;    // number of valid events
};