#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

bool initCc1101Radio();
bool isCc1101Ready();
float getCc1101FrequencyMhz();
void setCc1101FrequencyMhz(float mhz);

bool transmitCc1101(uint32_t code,
                    int bits,
                    int pulseLength,
                    int protocol,
                    int repeat,
                    String &errorOut);

void appendCc1101Info(JsonObject obj);
