#pragma once

#include <stdint.h>

namespace boardpins {

// LILYGO T-Embed CC1101 (T-Embed-CC1101) shared bus/power pins.
constexpr uint8_t kPowerEnable = 15;

constexpr uint8_t kTftCs = 41;
constexpr uint8_t kTftBacklight = 21;
constexpr uint8_t kSdCs = 13;
constexpr uint8_t kCc1101Cs = 12;

constexpr uint8_t kEncoderA = 4;
constexpr uint8_t kEncoderB = 5;
constexpr uint8_t kEncoderOk = 0;
constexpr uint8_t kEncoderBack = 6;

}  // namespace boardpins
