#include "openclaw_app.h"

#include <WiFi.h>

#include <vector>

#include "../core/cc1101_radio.h"
#include "../core/ble_manager.h"
#include "../core/gateway_client.h"
#include "../core/runtime_config.h"
#include "../core/wifi_manager.h"
#include "../ui/ui_shell.h"

namespace {

String boolLabel(bool value) {
  return value ? "Yes" : "No";
}

std::vector<String> buildStatusLines(AppContext &ctx) {
  std::vector<String> lines;

  GatewayStatus gs = ctx.gateway->status();
  String cfgErr;
  const bool configOk = validateConfig(ctx.config, &cfgErr);

  lines.push_back("Config Valid: " + boolLabel(configOk));
  if (!configOk) {
    lines.push_back("Setting app input required");
    lines.push_back("Config Error: " + cfgErr);
  }
  lines.push_back("Wi-Fi Connected: " + boolLabel(ctx.wifi->isConnected()));
  lines.push_back("Wi-Fi SSID: " + (ctx.wifi->ssid().isEmpty() ? String("(empty)") : ctx.wifi->ssid()));
  lines.push_back("IP: " + (ctx.wifi->ip().isEmpty() ? String("-") : ctx.wifi->ip()));
  lines.push_back("RSSI: " + String(ctx.wifi->rssi()));
  lines.push_back("Gateway URL: " + (ctx.config.gatewayUrl.isEmpty() ? String("(empty)") : ctx.config.gatewayUrl));
  lines.push_back("WS Connected: " + boolLabel(gs.wsConnected));
  lines.push_back("Gateway Ready: " + boolLabel(gs.gatewayReady));
  lines.push_back("Should Connect: " + boolLabel(gs.shouldConnect));
  lines.push_back("Auth Mode: " + String(gatewayAuthModeName(ctx.config.gatewayAuthMode)));
  lines.push_back("CC1101 Ready: " + boolLabel(isCc1101Ready()));
  lines.push_back("CC1101 Freq MHz: " + String(getCc1101FrequencyMhz(), 2));

  BleStatus bs = ctx.ble->status();
  lines.push_back("BLE Connected: " + boolLabel(bs.connected));
  lines.push_back("BLE Device: " +
                  (bs.deviceName.isEmpty() ? String("(none)") : bs.deviceName));
  lines.push_back("BLE Address: " +
                  (bs.deviceAddress.isEmpty() ? String("(none)") : bs.deviceAddress));
  if (bs.rssi != 0) {
    lines.push_back("BLE RSSI: " + String(bs.rssi));
  }
  if (!bs.lastError.isEmpty()) {
    lines.push_back("BLE Last Error: " + bs.lastError);
  }

  if (!gs.lastError.isEmpty()) {
    lines.push_back("Last Error: " + gs.lastError);
  }

  return lines;
}

}  // namespace

void runOpenClawApp(AppContext &ctx,
                    const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    const GatewayStatus gs = ctx.gateway->status();
    String subtitle = "Wi-Fi:";
    subtitle += ctx.wifi->isConnected() ? "UP " : "DOWN ";
    subtitle += "GW:";
    subtitle += gs.gatewayReady ? "READY" : (gs.wsConnected ? "WS" : "IDLE");

    std::vector<String> menu;
    menu.push_back("Status");
    menu.push_back("Connect");
    menu.push_back("Disconnect");
    menu.push_back("Reconnect");
    menu.push_back("Back");

    const int choice = ctx.ui->menuLoop("OpenClaw",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        subtitle);

    if (choice < 0 || choice == 4) {
      return;
    }

    selected = choice;

    if (choice == 0) {
      ctx.ui->showInfo("OpenClaw Status",
                       buildStatusLines(ctx),
                       backgroundTick,
                       "OK/BACK Exit");
      continue;
    }

    if (choice == 1) {
      String validateErr;
      if (!validateConfig(ctx.config, &validateErr)) {
        ctx.ui->showToast("Config Error", validateErr, 1800, backgroundTick);
        continue;
      }
      if (ctx.config.gatewayUrl.isEmpty()) {
        ctx.ui->showToast("Config Error",
                          "Set gateway URL first",
                          1600,
                          backgroundTick);
        continue;
      }
      ctx.gateway->configure(ctx.config);
      ctx.gateway->connectNow();
      ctx.ui->showToast("OpenClaw", "Connect requested", 1200, backgroundTick);
      continue;
    }

    if (choice == 2) {
      ctx.gateway->disconnectNow();
      ctx.ui->showToast("OpenClaw", "Disconnected", 1200, backgroundTick);
      continue;
    }

    if (choice == 3) {
      String validateErr;
      if (!validateConfig(ctx.config, &validateErr)) {
        ctx.ui->showToast("Config Error", validateErr, 1800, backgroundTick);
        continue;
      }
      ctx.gateway->configure(ctx.config);
      ctx.gateway->reconnectNow();
      ctx.ui->showToast("OpenClaw", "Reconnect requested", 1400, backgroundTick);
      continue;
    }
  }
}
