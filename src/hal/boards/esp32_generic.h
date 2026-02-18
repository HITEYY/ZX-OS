#pragma once

// ============================================================================
// ESP32-S3 Generic DevKit (headless or custom wiring)
// MCU: ESP32-S3  |  Flash: varies  |  PSRAM: varies
// Display: None (Serial/Web UI only)  |  Input: None (Serial only)
// RF: External CC1101 via SPI
// ============================================================================

#define HAL_BOARD_NAME        "ESP32-S3 Generic"
#define HAL_MCU               "ESP32-S3"

// --- Capabilities ---
#define HAL_HAS_DISPLAY       0
#define HAL_HAS_CC1101        0
#define HAL_HAS_ENCODER       0
#define HAL_HAS_BUTTONS       0
#define HAL_HAS_SD_CARD       0
#define HAL_HAS_PMU           0
#define HAL_HAS_BATTERY_GAUGE 0
#define HAL_HAS_SPEAKER       0
#define HAL_HAS_MIC           0
#define HAL_HAS_ANTENNA_SWITCH 0
#define HAL_HAS_POWER_ENABLE  0

// --- SPI Bus (default ESP32-S3 FSPI) ---
#define HAL_SPI_SCK           12
#define HAL_SPI_MISO          13
#define HAL_SPI_MOSI          11
#define HAL_SPI_FREQUENCY     10000000
#define HAL_SPI_READ_FREQ     10000000

// --- CC1101 RF (user-wired) ---
#define HAL_PIN_CC1101_CS     10
#define HAL_PIN_CC1101_GDO0   9

// --- Buttons (none) ---
#define HAL_PIN_BTN_OK        -1
#define HAL_PIN_BTN_BACK      -1

// --- I2C ---
#define HAL_I2C_SDA           -1
#define HAL_I2C_SCL           -1
