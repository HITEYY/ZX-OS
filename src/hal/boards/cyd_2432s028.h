#pragma once

// ============================================================================
// CYD (Cheap Yellow Display) ESP32-2432S028R
// MCU: ESP32 (classic, dual-core 240 MHz)  |  Flash: 4 MB  |  PSRAM: 0
// Display: ILI9341 320x240 (resistive touch)  |  Input: Touchscreen
// RF: No built-in CC1101 (external via SPI)
// ============================================================================

#define HAL_BOARD_NAME        "CYD ESP32-2432S028"
#define HAL_MCU               "ESP32"

// --- Capabilities ---
#define HAL_HAS_DISPLAY       1
#define HAL_HAS_CC1101        0
#define HAL_HAS_ENCODER       0
#define HAL_HAS_BUTTONS       0
#define HAL_HAS_TOUCH         1
#define HAL_HAS_SD_CARD       1
#define HAL_HAS_PMU           0
#define HAL_HAS_BATTERY_GAUGE 0
#define HAL_HAS_SPEAKER       0
#define HAL_HAS_MIC           0
#define HAL_HAS_ANTENNA_SWITCH 0
#define HAL_HAS_POWER_ENABLE  0

// --- Display (ILI9341, SPI) ---
#define HAL_DISPLAY_WIDTH     320
#define HAL_DISPLAY_HEIGHT    240
#define HAL_DISPLAY_ROTATION  1
#define HAL_PIN_TFT_CS        15
#define HAL_PIN_TFT_DC        2
#define HAL_PIN_TFT_RST       -1
#define HAL_PIN_TFT_BACKLIGHT 21
#define HAL_TFT_INVERSION_OFF 1

// --- SPI Bus ---
#define HAL_SPI_SCK           14
#define HAL_SPI_MISO          12
#define HAL_SPI_MOSI          13
#define HAL_SPI_FREQUENCY     40000000
#define HAL_SPI_READ_FREQ     20000000

// --- SD Card ---
#define HAL_PIN_SD_CS         5

// --- Touch (XPT2046, separate SPI) ---
#define HAL_PIN_TOUCH_CS      33
#define HAL_PIN_TOUCH_IRQ     36
#define HAL_TOUCH_SPI_SCK     25
#define HAL_TOUCH_SPI_MISO    39
#define HAL_TOUCH_SPI_MOSI    32

// --- CC1101 RF (external wiring) ---
#define HAL_PIN_CC1101_CS     -1
#define HAL_PIN_CC1101_GDO0   -1

// --- Buttons (mapped from touch) ---
#define HAL_PIN_BTN_OK        -1
#define HAL_PIN_BTN_BACK      -1

// --- I2C (no native I2C on CYD, optional external) ---
#define HAL_I2C_SDA           -1
#define HAL_I2C_SCL           -1

// --- RGB LED ---
#define HAL_PIN_LED_R         4
#define HAL_PIN_LED_G         16
#define HAL_PIN_LED_B         17
