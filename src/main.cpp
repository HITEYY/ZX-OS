#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#define XPOWERS_CHIP_BQ25896
#include <XPowersLib.h>

#include "apps/app_context.h"
#include "core/cc1101_radio.h"
#include "core/ble_manager.h"
#include "core/board_pins.h"
#include "core/gateway_client.h"
#include "core/node_command_handler.h"
#include "core/runtime_config.h"
#include "core/wifi_manager.h"
#include "ui/i18n.h"
#include "ui/ui_navigator.h"
#include "ui/ui_runtime.h"

namespace {

UiRuntime gUiRuntime;
UiNavigator gUiNav;
WifiManager gWifi;
GatewayClient gGateway;
BleManager gBle;
NodeCommandHandler gNodeHandler;
AppContext gAppContext;
XPowersPPM gPmu;
bool gSleepDetectionArmed = false;

constexpr unsigned long kDeepSleepHoldMs = 3000UL;
constexpr unsigned long kSleepReleaseDebounceMs = 80UL;
constexpr unsigned long kSleepReleasePollMs = 5UL;

void enableTopButtonWakeup() {
  const gpio_num_t wakePin = static_cast<gpio_num_t>(boardpins::kEncoderBack);
  rtc_gpio_init(wakePin);
  rtc_gpio_set_direction(wakePin, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en(wakePin);
  rtc_gpio_pulldown_dis(wakePin);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext0_wakeup(wakePin, 0);
}

void waitTopButtonReleased() {
  unsigned long releasedSince = 0;
  while (true) {
    const bool pressed = digitalRead(boardpins::kEncoderBack) == LOW;
    const unsigned long now = millis();

    if (pressed) {
      releasedSince = 0;
    } else if (releasedSince == 0) {
      releasedSince = now;
    } else if (now - releasedSince >= kSleepReleaseDebounceMs) {
      return;
    }

    delay(kSleepReleasePollMs);
  }
}

[[noreturn]] void enterDeepSleepNow() {
  Serial.println("[power] entering deep sleep");

  gGateway.disconnectNow();
  gBle.disconnectNow();
  gWifi.disconnect();

  pinMode(boardpins::kTftBacklight, OUTPUT);
  analogWrite(boardpins::kTftBacklight, 0);
  digitalWrite(boardpins::kTftBacklight, LOW);

  // ext0 wake level is LOW, so arm wake only after the button is released.
  waitTopButtonReleased();
  enableTopButtonWakeup();
  delay(120);
  Serial.flush();
  esp_deep_sleep_start();

  while (true) {
    delay(1000);
  }
}

void tickDeepSleepButton() {
  static unsigned long pressedAtMs = 0;

  const bool pressed = digitalRead(boardpins::kEncoderBack) == LOW;
  const unsigned long now = millis();

  if (!gSleepDetectionArmed) {
    if (!pressed) {
      gSleepDetectionArmed = true;
    }
    return;
  }

  if (!pressed) {
    pressedAtMs = 0;
    return;
  }

  if (pressedAtMs == 0) {
    pressedAtMs = now;
    return;
  }

  if (now - pressedAtMs >= kDeepSleepHoldMs) {
    enterDeepSleepNow();
  }
}

void runBackgroundTick() {
  tickDeepSleepButton();
  gWifi.tick();
  gGateway.tick();
  gBle.tick();
  gUiRuntime.tick();
}

void configureGatewayCallbacks() {
  gGateway.setInvokeRequestHandler([](const String &invokeId,
                                      const String &nodeId,
                                      const String &command,
                                      JsonObjectConst params) {
    gNodeHandler.handleInvoke(invokeId, nodeId, command, params);
  });

  gGateway.setTelemetryBuilder([](JsonObject payload) {
    appendCc1101Info(payload);
    payload["wifiConnected"] = WiFi.status() == WL_CONNECTED;
    payload["wifiRssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
    payload["ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("");
    payload["uptimeMs"] = millis();
  });
}

void initBoardPower() {
  // T-Embed CC1101 needs this rail enabled for TFT/backlight/radio domain.
  pinMode(boardpins::kPowerEnable, OUTPUT);
  digitalWrite(boardpins::kPowerEnable, HIGH);
  delay(30);

  // Ensure TFT backlight is enabled after cold boot/wakeup.
  pinMode(boardpins::kTftBacklight, OUTPUT);
  analogWrite(boardpins::kTftBacklight, 255);

  Wire.begin(8, 18);
  if (gPmu.init(Wire, 8, 18, BQ25896_SLAVE_ADDRESS)) {
    gPmu.resetDefault();
    gPmu.setChargeTargetVoltage(4208);
    gPmu.enableMeasure(PowersBQ25896::CONTINUOUS);
    Serial.println("[boot] pmu ready");
  } else {
    Serial.println("[boot] pmu init failed");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(400);

  const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  Serial.println("[boot] start");
  if (wakeCause == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("[boot] wake source: top button");
  }

  // Keep shared SPI devices deselected before any peripheral init.
  pinMode(boardpins::kTftCs, OUTPUT);
  digitalWrite(boardpins::kTftCs, HIGH);
  pinMode(boardpins::kSdCs, OUTPUT);
  digitalWrite(boardpins::kSdCs, HIGH);
  pinMode(boardpins::kCc1101Cs, OUTPUT);
  digitalWrite(boardpins::kCc1101Cs, HIGH);

  initBoardPower();

  Serial.println("[boot] ui.begin()");
  gUiRuntime.begin();
  gSleepDetectionArmed = digitalRead(boardpins::kEncoderBack) == HIGH;

  Serial.println("[boot] cc1101.init()");
  const bool ccReady = initCc1101Radio();
  if (!ccReady) {
    gUiRuntime.showToast("CC1101", "CC1101 not detected", 1500, []() {});
  }
  Serial.println(ccReady ? "[boot] cc1101 ready" : "[boot] cc1101 missing");

  ConfigLoadSource configLoadSource = ConfigLoadSource::Defaults;
  String loadErr;
  if (!loadConfig(gAppContext.config, &configLoadSource, nullptr, &loadErr)) {
    gAppContext.config = makeDefaultConfig();
  }
  Serial.printf("[boot] cfg.uiLanguage=%s\n", gAppContext.config.uiLanguage.c_str());
  gUiRuntime.setLanguage(uiLanguageFromConfigCode(gAppContext.config.uiLanguage));
  gUiRuntime.setTimezone(gAppContext.config.timezoneTz);

  gWifi.begin();
  gWifi.configure(gAppContext.config);

  gGateway.begin();
  gGateway.configure(gAppContext.config);
  configureGatewayCallbacks();

  gBle.begin();
  gBle.configure(gAppContext.config);

  gNodeHandler.setGatewayClient(&gGateway);

  gAppContext.wifi = &gWifi;
  gAppContext.gateway = &gGateway;
  gAppContext.ble = &gBle;
  gAppContext.uiRuntime = &gUiRuntime;
  gAppContext.uiNav = &gUiNav;
  gAppContext.configDirty = false;

  if (gAppContext.config.autoConnect &&
      !gAppContext.config.gatewayUrl.isEmpty() &&
      hasGatewayCredentials(gAppContext.config)) {
    gGateway.connectNow();
  }

  if (gAppContext.config.bleAutoConnect &&
      !gAppContext.config.bleDeviceAddress.isEmpty()) {
    gBle.connectToDevice(gAppContext.config.bleDeviceAddress,
                         gAppContext.config.bleDeviceName,
                         nullptr);
  }

  if (!loadErr.isEmpty()) {
    gUiRuntime.showToast("Config", loadErr, 1800, runBackgroundTick);
  } else if (configLoadSource == ConfigLoadSource::SdCard) {
    gUiRuntime.showToast("Config", "Loaded from SD", 900, runBackgroundTick);
  } else if (configLoadSource == ConfigLoadSource::Nvs) {
    gUiRuntime.showToast("Config", "Loaded from NVS", 900, runBackgroundTick);
  } else {
    gUiRuntime.showToast("Config", "Using default seeds", 900, runBackgroundTick);
  }
}

void loop() {
  gUiNav.runLauncher(gAppContext, runBackgroundTick);
}
