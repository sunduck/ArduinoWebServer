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
    logDebug("Soil sensor " + String(i) + ": " + String(value));

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
void waterValve(int id, int seconds) {
  if (id < 0 || id >= 4) {
    logDebug("Invalid valve ID: " + String(id));
    return;
  }

  if (pumpActive) {
    logDebug("Pump already active, rejecting watering request");
    return;
  }

  if (seconds < 1) seconds = 1;
  if (seconds > 300) seconds = 300;

  logDebug("Watering valve " + String(id) + " for " + String(seconds) + "s");

  digitalWrite(relay5vPins[7], LOW);   // pump ON (active LOW)
  digitalWrite(relay12vPins[id], HIGH); // valve ON (active HIGH)
  pumpActive = true;

  // Log watering event with snapshot of soil readings
  addSoilLog(lastSoilReadings, id, seconds);

  struct ValveArgs { int id; int seconds; };
  ValveArgs *args = new ValveArgs{id, seconds};

  xTaskCreatePinnedToCore(
    [](void *param) {
      ValveArgs *a = (ValveArgs*)param;
      vTaskDelay(a->seconds * 1000 / portTICK_PERIOD_MS);

      digitalWrite(relay12vPins[a->id], LOW); // Valve OFF
      digitalWrite(relay5vPins[7], HIGH);     // Pump OFF

      pumpActive = false;
      logDebug("Valve " + String(a->id) + " OFF, pump OFF after watering");

      delete a;
      vTaskDelete(NULL);
    },
    "ValveTimer",
    2048,
    args,
    1,
    NULL,
    1
  );
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
}

void loop() {
  // Intentionally empty - work is done in tasks and server callbacks
}
