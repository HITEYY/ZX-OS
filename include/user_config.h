#pragma once

// Compile-time default seeds (used only when NVS has no saved config).
#define USER_WIFI_SSID "REPLACE_WITH_WIFI_SSID"
#define USER_WIFI_PASSWORD "REPLACE_WITH_WIFI_PASSWORD"

// OpenClaw gateway URL: ws://host:port or wss://host:port
// Examples:
//   - Tailnet direct gateway: ws://100.100.0.20:18789
//   - Direct TLS endpoint:  wss://gateway.example.com:443
#define USER_GATEWAY_URL "REPLACE_WITH_GATEWAY_URL"

// Provide either token or password configured on the gateway.
#define USER_GATEWAY_TOKEN "REPLACE_WITH_GATEWAY_TOKEN"
#define USER_GATEWAY_PASSWORD ""

// Default auth mode seed: 0=token, 1=password
#define USER_GATEWAY_AUTH_MODE 0

// Optional APPMarket defaults.
// Format: owner/repo (example: myorg/myfirmware)
#define USER_APPMARKET_GITHUB_REPO "HITEYY/AI-cc1101"
// Release asset file name (example: t-embed-cc1101.bin)
#define USER_APPMARKET_RELEASE_ASSET ""

// Node identity shown in OpenClaw.
#define USER_OPENCLAW_DISPLAY_NAME "T-Embed CC1101"
#define USER_OPENCLAW_INSTANCE_ID "t-embed-cc1101"
// All outgoing chat/file messages are routed to this OpenClaw agent.
#define USER_OPENCLAW_DEFAULT_AGENT_ID "default"

// CC1101 defaults
#define USER_DEFAULT_RF_FREQUENCY_MHZ 433.92f

// Voice recording defaults (ADC microphone input)
// Set to a valid ADC-capable pin to enable MIC recording. Use -1 to disable.
#define USER_MIC_ADC_PIN -1
// Recommended range: 4000~22050
#define USER_MIC_SAMPLE_RATE 8000U
// Default recording duration for quick voice messages.
#define USER_MIC_DEFAULT_SECONDS 5U
// Maximum recording duration accepted by UI.
#define USER_MIC_MAX_SECONDS 30U

// External module pins (change for your wiring)
// PN532 NFC (I2C)
#define USER_NFC_I2C_SDA 8
#define USER_NFC_I2C_SCL 18
#define USER_NFC_IRQ_PIN 7
#define USER_NFC_RESET_PIN 42
// RC522 RFID (SPI shared with TFT/CC1101)
#define USER_RFID_SS_PIN 2
#define USER_RFID_RST_PIN 1
// nRF24L01 (SPI shared with TFT/CC1101)
#define USER_NRF24_CE_PIN 17
#define USER_NRF24_CSN_PIN 14
#define USER_NRF24_CHANNEL 76
// 0:250kbps, 1:1Mbps, 2:2Mbps
#define USER_NRF24_DATA_RATE 1
// 0:MIN, 1:LOW, 2:HIGH, 3:MAX
#define USER_NRF24_PA_LEVEL 1

// Telemetry event interval sent via node.event
#define USER_TELEMETRY_INTERVAL_MS 30000UL

// Default policy: manual connect from OpenClaw app
#define USER_AUTO_CONNECT_DEFAULT false

// Header clock (NTP)
#define USER_TIMEZONE_TZ "UTC0"
#define USER_NTP_SERVER_1 "pool.ntp.org"
#define USER_NTP_SERVER_2 "time.nist.gov"
// Periodic UNIX time fallback source (UTC). Used to correct clock drift.
#define USER_UNIX_TIME_SERVER_URL "https://worldclockapi.com/api/json/utc/now"

// Display backlight brightness percent (0~100)
#define USER_DISPLAY_BRIGHTNESS_PERCENT 100

// Battery gauge (BQ27220 over I2C on T-Embed CC1101)
#define USER_BATTERY_GAUGE_ENABLED 1
#define USER_BATTERY_GAUGE_ADDR 0x55
#define USER_BATTERY_GAUGE_SOC_REG 0x2C
#define USER_BATTERY_GAUGE_SDA 8
#define USER_BATTERY_GAUGE_SCL 18
