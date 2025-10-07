#include "LogManager.h"

LogManager::LogManager()
{
  mutex = xSemaphoreCreateMutex();
  clear();
}

void LogManager::addSoilEvent(uint8_t sensorId, int value)
{
  if (xSemaphoreTake(mutex, portMAX_DELAY))
  {
    Event event;
    event.timestamp = time(nullptr);

    switch (sensorId)
    {
    case 0:
      event.eventType = EVENT_SOIL_READINGS_0;
      break;
    case 1:
      event.eventType = EVENT_SOIL_READINGS_1;
      break;
    case 2:
      event.eventType = EVENT_SOIL_READINGS_2;
      break;
    case 3:
      event.eventType = EVENT_SOIL_READINGS_3;
      break;
    default:
      event.eventType = EVENT_UNKNOWN;
      break;
    }
    event.value = value;
    pushEvent(event);
    xSemaphoreGive(mutex);
  }
}

void LogManager::addWaterEvent(uint8_t valveId, int durationSec)
{
  if (xSemaphoreTake(mutex, portMAX_DELAY))
  {
    Event event;
    event.timestamp = time(nullptr);

    switch (valveId)
    {
    case 0:
      event.eventType = EVENT_WATERING_0;
      break;
    case 1:
      event.eventType = EVENT_WATERING_1;
      break;
    case 2:
      event.eventType = EVENT_WATERING_2;
      break;
    case 3:
      event.eventType = EVENT_WATERING_3;
      break;
    default:
      event.eventType = EVENT_UNKNOWN;
      break;
    }
    event.value = durationSec;
    pushEvent(event);
    xSemaphoreGive(mutex);
  }
}

void LogManager::pushEvent(const Event &event)
{
  log[head] = event;
  head = (head + 1) % MAX_LOGS;

  if (count < MAX_LOGS)
  {
    count++;
  }
  // If buffer full, head will overwrite oldest
}

int LogManager::getEventCount() const
{
  return count;
}

Event LogManager::getEvent(int index) const
{
  Event result = {0, EVENT_UNKNOWN, 0};
  if (xSemaphoreTake(mutex, portMAX_DELAY))
  {
    if (index >= 0 && index < count)
    {
      int pos = (head - count + index + MAX_LOGS) % MAX_LOGS;
      result = log[pos];
    }
    xSemaphoreGive(mutex);
  }
  return result;
}

String LogManager::getEventTypeName(event_type_t type)
{
  switch (type)
  {
  case EVENT_UNKNOWN:
    return "UNKNOWN";
  case EVENT_SOIL_READINGS_0:
    return "SOIL_READING_0";
  case EVENT_SOIL_READINGS_1:
    return "SOIL_READING_1";
  case EVENT_SOIL_READINGS_2:
    return "SOIL_READING_2";
  case EVENT_SOIL_READINGS_3:
    return "SOIL_READING_3";
  case EVENT_WATERING_0:
    return "WATERING_0";
  case EVENT_WATERING_1:
    return "WATERING_1";
  case EVENT_WATERING_2:
    return "WATERING_2";
  case EVENT_WATERING_3:
    return "WATERING_3";
  default:
    return "INVALID_TYPE";
  }
}

void LogManager::clear()
{
  if (xSemaphoreTake(mutex, portMAX_DELAY))
  {
    head = 0;
    count = 0;
    for (int i = 0; i < MAX_LOGS; i++)
    {
      log[i] = {0, EVENT_UNKNOWN, 0};
    }
    xSemaphoreGive(mutex);
  }
}
