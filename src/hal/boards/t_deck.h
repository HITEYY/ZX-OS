#pragma once

// ============================================================================
// LilyGo T-Deck
// MCU: ESP32-S3  |  Flash: 16 MB  |  PSRAM: 8 MB
// Display: ST7789 320x240  |  Input: Keyboard + Trackball
// RF: No built-in CC1101 (external via SPI)
// ============================================================================

#define HAL_BOARD_NAME        "LilyGo T-Deck"
#define HAL_MCU               "ESP32-S3"

// --- Capabilities ---
#define HAL_HAS_DISPLAY       1
#define HAL_HAS_CC1101        0
#define HAL_HAS_ENCODER       0
#define HAL_HAS_BUTTONS       1
#define HAL_HAS_KEYBOARD      1
#define HAL_HAS_TOUCH         0
#define HAL_HAS_SD_CARD       1
#define HAL_HAS_PMU           0
#define HAL_HAS_BATTERY_GAUGE 0
#define HAL_HAS_SPEAKER       1
#define HAL_HAS_MIC           1
#define HAL_HAS_ANTENNA_SWITCH 0
#define HAL_HAS_POWER_ENABLE  1
#define HAL_HAS_TRACKBALL     1

// --- Power ---
#define HAL_PIN_POWER_ENABLE  10

// --- Display (ST7789, SPI) ---
#define HAL_DISPLAY_WIDTH     320
#define HAL_DISPLAY_HEIGHT    240
#define HAL_DISPLAY_ROTATION  1
#define HAL_PIN_TFT_CS        12
#define HAL_PIN_TFT_DC        11
#define HAL_PIN_TFT_RST       -1
#define HAL_PIN_TFT_BACKLIGHT 42
#define HAL_TFT_INVERSION_ON  1

// --- Shared SPI Bus ---
#define HAL_SPI_SCK           40
#define HAL_SPI_MISO          38
#define HAL_SPI_MOSI          41
#define HAL_SPI_FREQUENCY     40000000
#define HAL_SPI_READ_FREQ     20000000

// --- SD Card ---
#define HAL_PIN_SD_CS         39

// --- CC1101 RF (external wiring) ---
#define HAL_PIN_CC1101_CS     -1
#define HAL_PIN_CC1101_GDO0   -1

// --- Trackball (acts as encoder/navigator) ---
#define HAL_PIN_TRACKBALL_UP     3
#define HAL_PIN_TRACKBALL_DOWN   15
#define HAL_PIN_TRACKBALL_LEFT   1
#define HAL_PIN_TRACKBALL_RIGHT  2
#define HAL_PIN_TRACKBALL_CLICK  0

// --- Buttons ---
#define HAL_PIN_BTN_OK        0
#define HAL_PIN_BTN_BACK      -1

// --- Keyboard ---
#define HAL_PIN_KB_INT        46
#define HAL_I2C_KB_ADDR       0x55

// --- I2C ---
#define HAL_I2C_SDA           18
#define HAL_I2C_SCL           8

// --- Audio: PDM Microphone ---
#define HAL_PIN_MIC_PDM_DATA  4
#define HAL_PIN_MIC_PDM_CLK   5

// --- Audio: I2S Speaker ---
#define HAL_PIN_I2S_BCLK      7
#define HAL_PIN_I2S_LRCLK     5
#define HAL_PIN_I2S_DOUT      6
