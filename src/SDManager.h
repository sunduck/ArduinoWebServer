#pragma once
#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>

// SD pins
#define SD_SCK   42
#define SD_MISO  1
#define SD_MOSI  2
#define SD_CS    41

extern bool sdAvailable;

void setupSD();
void dumpSoilLogsToSD();
