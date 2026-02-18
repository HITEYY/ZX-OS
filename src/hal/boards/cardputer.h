#pragma once

// ============================================================================
// M5Stack Cardputer
// MCU: ESP32-S3  |  Flash: 8 MB  |  PSRAM: 0 (no PSRAM)
// Display: ST7789 240x135  |  Input: Built-in keyboard
// RF: No built-in CC1101 (external via Grove/SPI)
// ============================================================================

#define HAL_BOARD_NAME        "M5Stack Cardputer"
#define HAL_MCU               "ESP32-S3"

// --- Capabilities ---
#define HAL_HAS_DISPLAY       1
#define HAL_HAS_CC1101        0
#define HAL_HAS_ENCODER       0
#define HAL_HAS_BUTTONS       0
#define HAL_HAS_KEYBOARD      1
#define HAL_HAS_SD_CARD       0
#define HAL_HAS_PMU           0
#define HAL_HAS_BATTERY_GAUGE 0
#define HAL_HAS_SPEAKER       1
#define HAL_HAS_MIC           1
#define HAL_HAS_ANTENNA_SWITCH 0
#define HAL_HAS_POWER_ENABLE  0
#define HAL_HAS_IR            1

// --- Display (ST7789, SPI) ---
#define HAL_DISPLAY_WIDTH     240
#define HAL_DISPLAY_HEIGHT    135
#define HAL_DISPLAY_ROTATION  1
#define HAL_PIN_TFT_CS        37
#define HAL_PIN_TFT_DC        34
#define HAL_PIN_TFT_RST       33
#define HAL_PIN_TFT_BACKLIGHT 38
#define HAL_TFT_INVERSION_ON  1

// --- SPI Bus ---
#define HAL_SPI_SCK           36
#define HAL_SPI_MISO          -1
#define HAL_SPI_MOSI          35
#define HAL_SPI_FREQUENCY     40000000
#define HAL_SPI_READ_FREQ     20000000

// --- CC1101 RF (external wiring via Grove port) ---
#define HAL_PIN_CC1101_CS     -1
#define HAL_PIN_CC1101_GDO0   -1

// --- Keyboard (M5Cardputer I2C keyboard) ---
#define HAL_PIN_KB_INT        -1
#define HAL_I2C_KB_ADDR       0x08

// --- Buttons (keyboard maps to virtual OK/BACK) ---
#define HAL_PIN_BTN_OK        -1
#define HAL_PIN_BTN_BACK      -1

// --- I2C ---
#define HAL_I2C_SDA           2
#define HAL_I2C_SCL           1

// --- Audio: I2S Speaker ---
#define HAL_PIN_I2S_BCLK      41
#define HAL_PIN_I2S_LRCLK     43
#define HAL_PIN_I2S_DOUT      42

// --- Audio: Microphone (I2S PDM) ---
#define HAL_PIN_MIC_PDM_DATA  46
#define HAL_PIN_MIC_PDM_CLK   43

// --- IR Transmitter ---
#define HAL_PIN_IR_TX         44
