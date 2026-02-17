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
// Release asset file name used by APPMarket.
// This repo's release workflow always publishes this stable alias.
#define USER_APPMARKET_RELEASE_ASSET "openclaw-t-embed-cc1101-latest.bin"

// Node identity shown in OpenClaw.
#define USER_OPENCLAW_DISPLAY_NAME "T-Embed CC1101"
#define USER_OPENCLAW_INSTANCE_ID "t-embed-cc1101"
// All outgoing chat/file messages are routed to this OpenClaw agent.
#define USER_OPENCLAW_DEFAULT_AGENT_ID "default"
// Messenger attachment routing policy (node role safe defaults).
// 0: Disable legacy msg.file/msg.voice meta+chunk fallback (recommended)
// 1: Enable legacy fallback when framed agent.request transfer fails.
#define USER_MESSENGER_ENABLE_LEGACY_MEDIA_FALLBACK 0
// Max binary bytes for agent.request framed attachments.
// Larger payloads auto-fallback to metadata text message.
#define USER_MESSENGER_BINARY_ATTACH_MAX_BYTES 524288U
// Max preview chars included in text fallback for text-like files.
#define USER_MESSENGER_TEXT_FALLBACK_PREVIEW_MAX_CHARS 4000U

// CC1101 defaults
#define USER_DEFAULT_RF_FREQUENCY_MHZ 433.92f

// Voice recording defaults
// ADC microphone input:
// Set to a valid ADC-capable pin to enable ADC MIC recording. Use -1 to disable.
#define USER_MIC_ADC_PIN -1
// PDM digital microphone input:
// Set both pins to valid GPIOs to enable PDM MIC recording. Use -1 to disable.
// LilyGo T-Embed CC1101 default onboard MIC pins: DATA=42, CLK=39.
#define USER_MIC_PDM_DATA_PIN 42
#define USER_MIC_PDM_CLK_PIN 39
// Recommended range: 4000~22050
#define USER_MIC_SAMPLE_RATE 8000U
// Default recording duration for quick voice messages.
#define USER_MIC_DEFAULT_SECONDS 5U
// Maximum recording duration accepted by UI.
#define USER_MIC_MAX_SECONDS 30U
// Optional BLE audio stream UUID filter (leave empty for auto-detect).
// Example (Nordic UART): service=6E400001-B5A3-F393-E0A9-E50E24DCCA9E
//                         char   =6E400003-B5A3-F393-E0A9-E50E24DCCA9E
#define USER_BLE_AUDIO_SERVICE_UUID ""
#define USER_BLE_AUDIO_CHAR_UUID ""

// Audio playback output (I2S speaker / amp).
// LilyGo T-Embed CC1101 example pinout:
// BCLK=46, LRCLK=40, DOUT=7
// Set pins to -1 to disable local audio playback output.
#define USER_AUDIO_I2S_BCLK_PIN 46
#define USER_AUDIO_I2S_LRCLK_PIN 40
#define USER_AUDIO_I2S_DOUT_PIN 7
// Range: 0~21
#define USER_AUDIO_PLAYBACK_VOLUME 12

// External module pins (change for your wiring)
// PN532 NFC (I2C)
#define USER_NFC_I2C_SDA 8
#define USER_NFC_I2C_SCL 18
// LilyGo reference wiring: IRQ=17, RST=45 (avoid MIC DATA GPIO42 conflict).
#define USER_NFC_IRQ_PIN 17
#define USER_NFC_RESET_PIN 45
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
#define USER_UNIX_TIME_SERVER_URL "http://worldtimeapi.org/api/timezone/Etc/UTC"

// Display backlight brightness percent (0~100)
#define USER_DISPLAY_BRIGHTNESS_PERCENT 100

// Enable periodic heap usage tracing on Serial (0:off, 1:on)
#define USER_MEM_TRACE_ENABLED 0
// Enable input raw trace logs (GPIO/encoder) on Serial (0:off, 1:on)
#define USER_INPUT_TRACE_ENABLED 0

// Input pins (LilyGo T-Embed CC1101 defaults)
// Change these if your board revision uses different encoder/button GPIOs.
#define USER_ENCODER_A_PIN 4
#define USER_ENCODER_B_PIN 5
#define USER_ENCODER_OK_PIN 0
#define USER_ENCODER_BACK_PIN 6

// Battery gauge (BQ27220 over I2C on T-Embed CC1101)
#define USER_BATTERY_GAUGE_ENABLED 1
#define USER_BATTERY_GAUGE_ADDR 0x55
#define USER_BATTERY_GAUGE_SOC_REG 0x2C
#define USER_BATTERY_GAUGE_SDA 8
#define USER_BATTERY_GAUGE_SCL 18
