#pragma once

// Compile-time default seeds (used only when NVS has no saved config).
#define USER_WIFI_SSID "REPLACE_WITH_WIFI_SSID"
#define USER_WIFI_PASSWORD "REPLACE_WITH_WIFI_PASSWORD"

// OpenClaw gateway URL: ws://host:port or wss://host:port
#define USER_GATEWAY_URL "ws://127.0.0.1:18789"

// Provide either token or password configured on the gateway.
#define USER_GATEWAY_TOKEN "REPLACE_WITH_GATEWAY_TOKEN"
#define USER_GATEWAY_PASSWORD ""

// Default auth mode seed: 0=token, 1=password
#define USER_GATEWAY_AUTH_MODE 0

// Node identity shown in OpenClaw.
#define USER_OPENCLAW_DISPLAY_NAME "T-Embed CC1101"
#define USER_OPENCLAW_INSTANCE_ID "t-embed-cc1101"

// CC1101 defaults
#define USER_DEFAULT_RF_FREQUENCY_MHZ 433.92f

// Telemetry event interval sent via node.event
#define USER_TELEMETRY_INTERVAL_MS 30000UL

// Default policy: manual connect from OpenClaw app
#define USER_AUTO_CONNECT_DEFAULT false

// Header clock (NTP)
#define USER_TIMEZONE_TZ "UTC0"
#define USER_NTP_SERVER_1 "pool.ntp.org"
#define USER_NTP_SERVER_2 "time.nist.gov"

// Battery gauge (BQ27220 over I2C on T-Embed CC1101)
#define USER_BATTERY_GAUGE_ENABLED 1
#define USER_BATTERY_GAUGE_ADDR 0x55
#define USER_BATTERY_GAUGE_SOC_REG 0x2C
#define USER_BATTERY_GAUGE_SDA 8
#define USER_BATTERY_GAUGE_SCL 18
