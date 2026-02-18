#pragma once

#include <stdint.h>

#include "user_config.h"
#include "../hal/board_config.h"

namespace boardpins {

// Power enable (boards without this pin define HAL_PIN_POWER_ENABLE as absent).
#if HAL_HAS_POWER_ENABLE
constexpr uint8_t kPowerEnable = HAL_PIN_POWER_ENABLE;
#endif

// Display
#if HAL_HAS_DISPLAY
constexpr int8_t kTftCs = HAL_PIN_TFT_CS;
constexpr int8_t kTftBacklight = HAL_PIN_TFT_BACKLIGHT;
#endif

// SD Card
#if HAL_HAS_SD_CARD
constexpr int8_t kSdCs = HAL_PIN_SD_CS;
#endif

// CC1101 Radio
#if HAL_HAS_CC1101
constexpr int8_t kCc1101Cs = HAL_PIN_CC1101_CS;
#endif

// Shared SPI bus
#if defined(HAL_SPI_SCK) && HAL_SPI_SCK >= 0
constexpr uint8_t kSpiSck = HAL_SPI_SCK;
constexpr uint8_t kSpiMiso = HAL_SPI_MISO;
constexpr uint8_t kSpiMosi = HAL_SPI_MOSI;
#endif

// Rotary encoder (T-Embed CC1101 style)
#if HAL_HAS_ENCODER
constexpr uint8_t kEncoderA = static_cast<uint8_t>(USER_ENCODER_A_PIN);
constexpr uint8_t kEncoderB = static_cast<uint8_t>(USER_ENCODER_B_PIN);
#endif

// OK / BACK buttons (may be physical pins or -1 if virtual/touch)
#if defined(HAL_PIN_BTN_OK) && HAL_PIN_BTN_OK >= 0
constexpr uint8_t kBtnOk = static_cast<uint8_t>(HAL_PIN_BTN_OK);
#endif
#if defined(HAL_PIN_BTN_BACK) && HAL_PIN_BTN_BACK >= 0
constexpr uint8_t kBtnBack = static_cast<uint8_t>(HAL_PIN_BTN_BACK);
#endif

// Legacy aliases for existing code that references kEncoderOk / kEncoderBack
#if HAL_HAS_ENCODER
constexpr uint8_t kEncoderOk = static_cast<uint8_t>(USER_ENCODER_OK_PIN);
constexpr uint8_t kEncoderBack = static_cast<uint8_t>(USER_ENCODER_BACK_PIN);
#elif defined(HAL_PIN_BTN_OK) && HAL_PIN_BTN_OK >= 0
constexpr uint8_t kEncoderOk = static_cast<uint8_t>(HAL_PIN_BTN_OK);
#if defined(HAL_PIN_BTN_BACK) && HAL_PIN_BTN_BACK >= 0
constexpr uint8_t kEncoderBack = static_cast<uint8_t>(HAL_PIN_BTN_BACK);
#else
constexpr uint8_t kEncoderBack = static_cast<uint8_t>(HAL_PIN_BTN_OK);
#endif
#endif

// I2C
#if defined(HAL_I2C_SDA) && HAL_I2C_SDA >= 0
constexpr uint8_t kI2cSda = HAL_I2C_SDA;
constexpr uint8_t kI2cScl = HAL_I2C_SCL;
#endif

// Trackball (T-Deck)
#if defined(HAL_HAS_TRACKBALL) && HAL_HAS_TRACKBALL
constexpr uint8_t kTrackballUp = HAL_PIN_TRACKBALL_UP;
constexpr uint8_t kTrackballDown = HAL_PIN_TRACKBALL_DOWN;
constexpr uint8_t kTrackballLeft = HAL_PIN_TRACKBALL_LEFT;
constexpr uint8_t kTrackballRight = HAL_PIN_TRACKBALL_RIGHT;
constexpr uint8_t kTrackballClick = HAL_PIN_TRACKBALL_CLICK;
#endif

}  // namespace boardpins
