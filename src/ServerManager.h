#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>


// Externally defined in main.cpp
extern AsyncWebServer server;


// Setup all REST endpoints
void setupServer();