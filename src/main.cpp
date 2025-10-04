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
// ESP32 Garden Controller - main.cpp
// Restored missing helpers: setupWiFi, setupNTP, readSoilSensors, soilTask, waterValve
// ===============================================================

const int relay5vPins[8] = {18, 17, 16, 15, 7, 6, 5, 4};
const int relay12vPins[4] = {47, 21, 20, 19};
const int soilPins[4]     = {10, 9, 11, 3};

int lastSoilReadings[4] = {0, 0, 0, 0};
AsyncWebServer server(80);
ConfigManager config;
volatile bool pumpActive = false;  // guard: only one watering at a time

String getTimestamp() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

void logDebug(const String &msg) {
  Serial0.print("[");
  Serial0.print(getTimestamp());
  Serial0.print("] ");
  Serial0.println(msg);
}

// --- WiFi + NTP ---
void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial0.print("Connecting to WiFi");
  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial0.print(".");
    if (millis() - startAttempt > 30000) {
      Serial0.println("\n[ERROR] WiFi connection failed, rebooting...");
      ESP.restart();
    }
  }

  Serial0.println();
  Serial0.print("Connected! IP: ");
  Serial0.println(WiFi.localIP());
}

void setupNTP() {
  // Adjust timezone offset if needed. Currently +3 hours.
  configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  struct tm timeinfo;
  Serial0.print("Syncing time via NTP");
  unsigned long startAttempt = millis();

  while (!getLocalTime(&timeinfo)) {
    delay(500);
    Serial0.print(".");
    if (millis() - startAttempt > 30000) {
      Serial0.println("\n[ERROR] NTP sync failed, rebooting...");
      ESP.restart();
    }
  }

  Serial0.println();
  Serial0.println("NTP time synced successfully");
  logDebug("NTP sync successful, timestamped logging enabled");
}

// --- Soil sensors ---
void readSoilSensors() {
  const int samples = 5;
  for (int i = 0; i < 4; i++) {
    digitalWrite(relay5vPins[i], LOW); // enable 5V to sensor (active LOW on your board)
    delay(config.sensorSettleTime);

    long sum = 0;
    for (int j = 0; j < samples; j++) {
      sum += analogRead(soilPins[i]);
      delay(50);
    }
    int value = sum / samples;
    lastSoilReadings[i] = value;
    //logDebug("Soil sensor " + String(i) + ": " + String(value));

    digitalWrite(relay5vPins[i], HIGH); // disable sensor
  }
}

// --- Soil task ---
void soilTask(void *pvParameters) {
  int lastHour = -1;

  for (;;) {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    // Daily rollover at lightStart (fire once when hour changes to lightStart)
    if (timeinfo.tm_hour == config.lightStart && timeinfo.tm_hour != lastHour) {
      resetSoilLogs();
    }
    lastHour = timeinfo.tm_hour;

    int startHour = (config.lightStart - 1 + 24) % 24; // start one hour earlier
    int endHour   = config.lightEnd;

    bool inLightCycle;
    if (startHour < endHour) {
      inLightCycle = (timeinfo.tm_hour >= startHour && timeinfo.tm_hour < endHour);
    } else {
      inLightCycle = (timeinfo.tm_hour >= startHour || timeinfo.tm_hour < endHour);
    }

    if (inLightCycle && (config.soilLogIntervalMin > 0) && (timeinfo.tm_min % config.soilLogIntervalMin == 0)) {
      readSoilSensors();
      addSoilLog(lastSoilReadings);   // normal log, no watering event
      vTaskDelay(60000 / portTICK_PERIOD_MS); // avoid duplicate logs within same minute
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// --- Watering control ---
void waterValve(int duration0, int duration1, int duration2, int duration3) {

  if (pumpActive) {
    logDebug("Pump already active, rejecting watering request");
    return;
  }

  pumpActive = true;

  int ValveDurations[4] = {duration0, duration1, duration2, duration3};
  int* durations = new int[4];
  memcpy(durations, ValveDurations, sizeof(ValveDurations));


  xTaskCreatePinnedToCore(
    [](void *param) {
      int* durations = (int*)param;
      for (int i = 0; i < 4; i++) {
        int seconds = durations[i];
        //logDebug("Valve " + String(i) + " for " + String(seconds) + "s");
        if (seconds > 0) {
          //logDebug("Watering valve " + String(i) + " for " + String(seconds) + "s");
          digitalWrite(relay12vPins[i], HIGH); // valve ON (active HIGH)
          digitalWrite(relay5vPins[7], LOW);   // pump ON (active LOW)
          vTaskDelay((seconds + 5) * 1000 / portTICK_PERIOD_MS); // wait for valve + pump to finish, add buffer
          digitalWrite(relay5vPins[7], HIGH);     // pump OFF
          digitalWrite(relay12vPins[i], LOW); // Valve OFF
          //logDebug("Valve " + String(i) + " OFF, pump OFF after watering");
          vTaskDelay (5000 / portTICK_PERIOD_MS); // wait 5s before next valve
        }
      }
      pumpActive = false;
      //logDebug("Watering task completed, pump OFF");
      delete[] durations; // Free memory after use
      vTaskDelete(NULL);
    },
    "WateringTask",
    4096,
    durations,
    1,NULL,
    1 

  );
}

void wateringTask(void *pvParameters) {
  int lastMinute = -1;
  TickType_t lastWake = xTaskGetTickCount();

  for (;;) {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_min != lastMinute) {
      lastMinute = timeinfo.tm_min;

      char buf[6];
      strftime(buf, sizeof(buf), "%H:%M", &timeinfo);

      for (const auto &sched : config.wateringSchedules) {
        if (sched.time == String(buf)) {
          
          //Serial0.printf("[WateringTask] Schedule triggered: %s\n", sched.time.c_str());
          waterValve(sched.durations[0], sched.durations[1], sched.durations[2], sched.durations[3]);
          for (int i = 0; i < 4; i++) {
            if (sched.durations[i] > 0) {
              //logDebug("Scheduled watering: Valve " + String(i) + " for " + String(sched.durations[i]) + "s");
              addSoilLog(lastSoilReadings, i, sched.durations[i]); // log with watering event
            }
          }
           // log with watering event
        }
      }
    }
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1000));
  }
}

// --- Setup + loop ---
void setup() {
  Serial0.begin(115200);

  for (int i = 0; i < 8; i++) {
    pinMode(relay5vPins[i], OUTPUT);
    digitalWrite(relay5vPins[i], HIGH); // default OFF (active LOW)
  }
  Serial0.println("[DEBUG] 5V relays initialized (default OFF, active LOW)");

  for (int i = 0; i < 4; i++) {
    pinMode(relay12vPins[i], OUTPUT);
    digitalWrite(relay12vPins[i], LOW); // default OFF (active HIGH)
  }
  Serial0.println("[DEBUG] 12V relays initialized (default OFF, active HIGH)");

  for (int i = 0; i < 4; i++) pinMode(soilPins[i], INPUT);
  Serial0.println("[DEBUG] Soil sensor pins set as INPUT");

  setupWiFi();
  setupNTP();

  config.load();

  // Register routes (ServerManager.cpp)
  setupServer();

  // Start web server after all modules had chance to register endpoints
  server.begin();
  logDebug("Web server started");

  if (config.soilLogIntervalMin <= 0) config.soilLogIntervalMin = 15; // safety default

  // Start soil logging task (pinned to core 1)
  xTaskCreatePinnedToCore(soilTask, "SoilTask", 4096, NULL, 1, NULL, 1);

  xTaskCreatePinnedToCore(wateringTask, "WateringTask", 4096, NULL, 1, NULL, 1);
}

void loop() {
  // Intentionally empty - work is done in tasks and server callbacks
}
