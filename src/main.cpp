#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Wire.h>

#define XPOWERS_CHIP_BQ25896
#include <XPowersLib.h>

#include <vector>

#include "apps/app_market_app.h"
#include "apps/app_context.h"
#include "apps/file_explorer_app.h"
#include "apps/openclaw_app.h"
#include "apps/settings_app.h"
#include "apps/tailscale_app.h"
#include "core/cc1101_radio.h"
#include "core/ble_manager.h"
#include "core/board_pins.h"
#include "core/gateway_client.h"
#include "core/node_command_handler.h"
#include "core/runtime_config.h"
#include "core/tailscale_lite_client.h"
#include "core/wifi_manager.h"
#include "ui/ui_shell.h"

namespace {

UIShell gUi;
WifiManager gWifi;
GatewayClient gGateway;
BleManager gBle;
TailscaleLiteClient gTailscaleLite;
NodeCommandHandler gNodeHandler;
AppContext gAppContext;
XPowersPPM gPmu;

void runBackgroundTick() {
  gWifi.tick();
  gTailscaleLite.tick();
  gGateway.tick();
  gBle.tick();
}

String buildLauncherStatus() {
  String line;
  line += gWifi.isConnected() ? "WiFi:UP " : "WiFi:DOWN ";

  const GatewayStatus gs = gGateway.status();
  line += "GW:";
  line += gs.gatewayReady ? "READY" : (gs.wsConnected ? "WS" : "IDLE");
  line += " TS:";
  line += gTailscaleLite.isConnected() ? "UP" : "DOWN";
  line += " BLE:";
  line += gBle.isConnected() ? "CONN" : "IDLE";

  if (gAppContext.configDirty) {
    line += "  *DIRTY";
  }

  return line;
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

void runLauncher() {
  static int selected = 0;

  std::vector<String> items;
  items.push_back("OpenClaw");
  items.push_back("Setting");
  items.push_back("File Explorer");
  items.push_back("Tailscale");
  items.push_back("APPMarket");

  gUi.setStatusLine(buildLauncherStatus());
  const int choice = gUi.menuLoop("Launcher",
                                  items,
                                  selected,
                                  runBackgroundTick,
                                  "OK Select",
                                  "T-Embed CC1101");
  if (choice < 0) {
    return;
  }

  selected = choice;
  if (choice == 0) {
    runOpenClawApp(gAppContext, runBackgroundTick);
  } else if (choice == 1) {
    runSettingsApp(gAppContext, runBackgroundTick);
  } else if (choice == 2) {
    runFileExplorerApp(gAppContext, runBackgroundTick);
  } else if (choice == 3) {
    runTailscaleApp(gAppContext, runBackgroundTick);
  } else if (choice == 4) {
    runAppMarketApp(gAppContext, runBackgroundTick);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(400);

  Serial.println("[boot] start");

  // Keep shared SPI devices deselected before any peripheral init.
  pinMode(boardpins::kTftCs, OUTPUT);
  digitalWrite(boardpins::kTftCs, HIGH);
  pinMode(boardpins::kSdCs, OUTPUT);
  digitalWrite(boardpins::kSdCs, HIGH);
  pinMode(boardpins::kCc1101Cs, OUTPUT);
  digitalWrite(boardpins::kCc1101Cs, HIGH);

  initBoardPower();

  Serial.println("[boot] ui.begin()");
  gUi.begin();

  Serial.println("[boot] cc1101.init()");
  const bool ccReady = initCc1101Radio();
  if (!ccReady) {
    gUi.showToast("CC1101", "CC1101 not detected", 1500, []() {});
  }
  Serial.println(ccReady ? "[boot] cc1101 ready" : "[boot] cc1101 missing");

  ConfigLoadSource configLoadSource = ConfigLoadSource::Defaults;
  String loadErr;
  if (!loadConfig(gAppContext.config, &configLoadSource, nullptr, &loadErr)) {
    gAppContext.config = makeDefaultConfig();
  }

  gWifi.begin();
  gWifi.configure(gAppContext.config);

  gGateway.begin();
  gGateway.configure(gAppContext.config);
  configureGatewayCallbacks();

  gBle.begin();
  gBle.configure(gAppContext.config);
  gTailscaleLite.begin();
  gTailscaleLite.configure(gAppContext.config);

  gNodeHandler.setGatewayClient(&gGateway);

  gAppContext.wifi = &gWifi;
  gAppContext.gateway = &gGateway;
  gAppContext.ble = &gBle;
  gAppContext.tailscaleLite = &gTailscaleLite;
  gAppContext.ui = &gUi;
  gAppContext.configDirty = false;

  if (gAppContext.config.tailscaleLiteEnabled) {
    String liteErr;
    if (!gTailscaleLite.connectNow(&liteErr) && !liteErr.isEmpty()) {
      Serial.println("[boot] tailscale-lite: " + liteErr);
    }
  }

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
    gUi.showToast("Config", loadErr, 1800, runBackgroundTick);
  } else if (configLoadSource == ConfigLoadSource::SdCard) {
    gUi.showToast("Config", "Loaded from SD", 900, runBackgroundTick);
  } else if (configLoadSource == ConfigLoadSource::Nvs) {
    gUi.showToast("Config", "Loaded from NVS", 900, runBackgroundTick);
  } else {
    gUi.showToast("Config", "Using default seeds", 900, runBackgroundTick);
  }
}

void loop() {
  runLauncher();
}
