#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <time.h>
#include "WiFiCredentials.h"
#include "ConfigManager.h"
#include "SoilLogManager.h"
#include "ServerManager.h"

// ===============================================================
// ESP32 Uncle Sunduck Garden Controller
//
// Idea of the system is to water 4 plants fixed amount of times per defined light cycle
// and log soil moisture during light cycle and power only at the times we gather data
// to prevent corrosion of soil sensors electrodes (full-time power will end them in a month).
// 
// Amount of water per valve and watering times can be configured via web interface
// by analyzing soil moisture logs and drainage volume. 
// Later maybe going full automatic by soil moisture sensors only.
//
// Hardware: ESP32-S3 N16R8 - some cheap aliexpess knockoff board with 16MB flash, it works fine
//           4x5V resistive soil moisture sensors
//           8x5v relay board (4 relays to power up soil sensors, for 12v membrane water pump), 
//           4x12V relay board (for 4 12v water valves) 
//
//                                     !!! IMPORTANT !!!
//           Don't forget to shunt those 12V valves with a diodes (1N4007 or similar) 
// Otherwise EMI from discharging valve coil will corrupt your ESP32 flash on valve powering off! 
//    (Seriously, i lost a lot of time to find this out, i slept on electrotechnics lectures)
//                                     !!!!!!!!!!!!!!!!!
//
// Control via REST on web interface, config stored in flash - it won't loose config on power loss
// Currently my system can supply ~15 ml/s flow rate.
// ===============================================================

// --- Pin definitions ---
const int relay5vPins[8] = {18, 17, 16, 15, 7, 6, 5, 4};
const int relay12vPins[4] = {47, 21, 20, 19};
const int soilPins[4] = {10, 9, 11, 3};

int lastSoilReadings[4] = {0, 0, 0, 0};
AsyncWebServer server(80);
ConfigManager config;
volatile bool pumpActive = false; // guard: only one watering at a time

String getTimestamp()
{
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

void logDebug(const String &msg)
{
  Serial0.print("[");
  Serial0.print(getTimestamp());
  Serial0.print("] ");
  Serial0.println(msg);
}

// --- WiFi + NTP ---
void setupWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial0.print("Connecting to WiFi");
  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial0.print(".");
    if (millis() - startAttempt > 30000)
    {
      Serial0.println("\n[ERROR] WiFi connection failed, rebooting...");
      ESP.restart();
    }
  }

  Serial0.println();
  Serial0.print("Connected! IP: ");
  Serial0.println(WiFi.localIP());
}

void setupNTP()
{
  // Adjust timezone offset if needed. Currently +3 hours.
  configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  struct tm timeinfo;
  Serial0.print("Syncing time via NTP");
  unsigned long startAttempt = millis();

  while (!getLocalTime(&timeinfo))
  {
    delay(500);
    Serial0.print(".");
    if (millis() - startAttempt > 30000)
    {
      Serial0.println("\n[ERROR] NTP sync failed, rebooting...");
      ESP.restart();
    }
  }

  Serial0.println();
  Serial0.println("NTP time synced successfully");
  logDebug("NTP sync successful, timestamped logging enabled");
}

// --- Soil sensors ---
void readSoilSensors()
{
  const int samples = 5;
  for (int i = 0; i < 4; i++)
  {
    // powering up 5V sensor (active LOW)
    digitalWrite(relay5vPins[i], LOW); // powering up 5V sensor (active LOW)
    delay(config.sensorSettleTime);

    long sum = 0;
    for (int j = 0; j < samples; j++)
    {
      // reading sensor multiple times and averaging
      sum += analogRead(soilPins[i]);
      delay(50);
    }
    int value = sum / samples;
    lastSoilReadings[i] = value;
    // logDebug("Soil sensor " + String(i) + ": " + String(value));
    // powering down 5V sensor
    digitalWrite(relay5vPins[i], HIGH); // disable sensor
  }
}

/*
// Task scheduling soil moisture readings
// during light cycle only (to prevent corrosion of soil sensors)
// pinned to core 1 (not using WiFi functions)
// uses config values: 
// lightStart - start of the light cycle (it could start at night if you have night energy tatiffs) 
// lightEnd - end of the light cycle, it defines soil moisture logging period
// soilLogIntervalMin - interval in minutes to log soil data (e.g. int(15) is for every 15th minute of the hour)
*/

void soilTask(void *pvParameters)
{
  static int LogResetDay = -1;

  for (;;)
  {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    int startHour = (config.lightStart - 1 + 24) % 24; // start one hour earlier

    // Daily logs rollover at one hour before lightStart
    if (timeinfo.tm_hour == startHour && timeinfo.tm_min == 0 && timeinfo.tm_sec < 10 && timeinfo.tm_mday != LogResetDay)
    {
      resetSoilLogs();
      LogResetDay = timeinfo.tm_mday;
    }

    int endHour = config.lightEnd;

    bool inLightCycle;
    if (startHour < endHour)
    {
      inLightCycle = (timeinfo.tm_hour >= startHour && timeinfo.tm_hour < endHour);
    }
    else
    {
      inLightCycle = (timeinfo.tm_hour >= startHour || timeinfo.tm_hour < endHour);
    }

    if (inLightCycle && (config.soilLogIntervalMin > 0) && (timeinfo.tm_min % config.soilLogIntervalMin == 0))
    {
      readSoilSensors();
      addSoilLog(lastSoilReadings);           // normal log, no watering event
      vTaskDelay(60000 / portTICK_PERIOD_MS); // avoid duplicate logs within same minute
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void wateringCycle(int duration0, int duration1, int duration2, int duration3)
{

  if (pumpActive)
  {
    logDebug("Pump already active, rejecting watering request");
    return;
  }

  pumpActive = true;

  // Local fixed-size array — lives on the stack, fast and safe
  int durations[4] = {duration0, duration1, duration2, duration3};

  // Create a task; pass the array by *value* (copied into task stack)
  xTaskCreatePinnedToCore(
      [](void *param)
      {
        // Copy the array contents immediately, since param points to stack memory
        int localDurations[4];
        memcpy(localDurations, param, sizeof(localDurations));

        for (int i = 0; i < 4; i++)
        {
          int seconds = localDurations[i];
          if (seconds > 0)
          {
            digitalWrite(relay12vPins[i], HIGH); // Valve ON (active HIGH)
            digitalWrite(relay5vPins[7], LOW);   // Pump ON (active LOW)

            // Wait valve duration + 3s buffer
            vTaskDelay((seconds + 3) * 1000 / portTICK_PERIOD_MS);

            digitalWrite(relay5vPins[7], HIGH); // Pump OFF
            digitalWrite(relay12vPins[i], LOW); // Valve OFF

            // Wait 5 seconds before next valve
            vTaskDelay(5000 / portTICK_PERIOD_MS);
          }
        }

        pumpActive = false;
        vTaskDelete(NULL); // End task safely
      },
      "WCycleTask", // Task name
      4096,           // Stack size (bytes)
      durations,      // Parameter (copied in)
      1,              // Priority
      NULL,           // Task handle
      1               // Core (optional)
  );
}

void wateringSchedulerTask(void *pvParameters)
{
  int lastMinute = -1;
  TickType_t lastWake = xTaskGetTickCount();

  for (;;)
  {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_min != lastMinute)
    {
      lastMinute = timeinfo.tm_min;

      char buf[6];
      strftime(buf, sizeof(buf), "%H:%M", &timeinfo);

      for (const auto &sched : config.wateringSchedules)
      {
        if (sched.time == String(buf))
        {
          wateringCycle(sched.durations[0], sched.durations[1], sched.durations[2], sched.durations[3]);
          for (int i = 0; i < 4; i++)
          {
            if (sched.durations[i] > 0)
            {
              addSoilLog(lastSoilReadings, i, sched.durations[i]); // log with watering event
            }
          }
        }
      }
    }
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1000));
  }
}

// --- Setup + loop ---
void setup()
{
  Serial0.begin(115200);

  // Relay initialithation is specific to my hardware setup
  // some relay boards are active HIGH, some active LOW

  for (int i = 0; i < 8; i++)
  {
    pinMode(relay5vPins[i], OUTPUT);
    digitalWrite(relay5vPins[i], HIGH); // default OFF (active LOW)
  }
  Serial0.println("[DEBUG] 5V relays initialized (default OFF, active LOW)");

  for (int i = 0; i < 4; i++)
  {
    pinMode(relay12vPins[i], OUTPUT);
    digitalWrite(relay12vPins[i], LOW); // default OFF (active HIGH)
  }
  Serial0.println("[DEBUG] 12V relays initialized (default OFF, active HIGH)");

  for (int i = 0; i < 4; i++)
    pinMode(soilPins[i], INPUT);
  Serial0.println("[DEBUG] Soil sensor pins set as INPUT");

  setupWiFi();
  setupNTP();

  config.load();

  // Register routes (ServerManager.cpp)
  setupServer();

  // Start web server after all modules had chance to register endpoints
  server.begin();
  logDebug("Web server started");

  if (config.soilLogIntervalMin <= 0)
    config.soilLogIntervalMin = 15; // safety default

  // Start soil logging task (pinned to core 1)
  xTaskCreatePinnedToCore(soilTask, "SoilTask", 4096, NULL, 1, NULL, 1);

  xTaskCreatePinnedToCore(wateringSchedulerTask, "WSchedulerTask", 4096, NULL, 1, NULL, 1);
}

void loop()
{
  // Intentionally empty - work is done in tasks and server callbacks
}
