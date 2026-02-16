#pragma once

#include <Arduino.h>

#include <functional>

bool isMicRecordingAvailable();

bool recordMicWavToSd(const String &path,
                      uint16_t seconds,
                      const std::function<void()> &backgroundTick,
                      const std::function<bool()> &stopRequested = std::function<bool()>(),
                      String *error = nullptr,
                      uint32_t *bytesWritten = nullptr);
