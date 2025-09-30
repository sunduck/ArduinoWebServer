#pragma once
#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>

extern bool sdAvailable;

void setupSD();
void dumpSoilLogsToSD();

// File upload helper
bool saveUploadedFile(const String &filename, size_t index, uint8_t *data, size_t len, bool final);
