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

void markDirty(AppContext &ctx) {
  ctx.configDirty = true;
}

void runGatewayMenu(AppContext &ctx,
                    const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    menu.push_back("Edit URL");
    menu.push_back("Auth Mode");
    menu.push_back("Edit Credential");
    menu.push_back("Clear Gateway");
    menu.push_back("Back");

    String subtitle = "Auth: ";
    subtitle += gatewayAuthModeName(ctx.config.gatewayAuthMode);

    const int choice = ctx.ui->menuLoop("OpenClaw / Gateway",
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
      String url = ctx.config.gatewayUrl;
      if (ctx.ui->textInput("Gateway URL", url, false, backgroundTick)) {
        ctx.config.gatewayUrl = url;
        markDirty(ctx);
      }
      continue;
    }

    if (choice == 1) {
      std::vector<String> authItems;
      authItems.push_back("Token");
      authItems.push_back("Password");

      const int current = ctx.config.gatewayAuthMode == GatewayAuthMode::Password ? 1 : 0;
      const int authChoice = ctx.ui->menuLoop("Gateway Auth",
                                              authItems,
                                              current,
                                              backgroundTick,
                                              "OK Select  BACK Exit",
                                              "Choose auth mode");
      if (authChoice >= 0) {
        ctx.config.gatewayAuthMode = authChoice == 1
                                         ? GatewayAuthMode::Password
                                         : GatewayAuthMode::Token;
        markDirty(ctx);
      }
      continue;
    }

    if (choice == 2) {
      if (ctx.config.gatewayAuthMode == GatewayAuthMode::Password) {
        String password = ctx.config.gatewayPassword;
        if (ctx.ui->textInput("Gateway Password", password, true, backgroundTick)) {
          ctx.config.gatewayPassword = password;
          markDirty(ctx);
        }
      } else {
        String token = ctx.config.gatewayToken;
        if (ctx.ui->textInput("Gateway Token", token, true, backgroundTick)) {
          ctx.config.gatewayToken = token;
          markDirty(ctx);
        }
      }
      continue;
    }

    if (choice == 3) {
      ctx.config.gatewayUrl = "";
      ctx.config.gatewayToken = "";
      ctx.config.gatewayPassword = "";
      markDirty(ctx);
      ctx.ui->showToast("Gateway", "Gateway config cleared", 1200, backgroundTick);
      continue;
    }
  }
}

void applyRuntimeConfig(AppContext &ctx,
                        const std::function<void()> &backgroundTick) {
  String validateErr;
  if (!validateConfig(ctx.config, &validateErr)) {
    ctx.ui->showToast("Validation", validateErr, 1800, backgroundTick);
    return;
  }

  String saveErr;
  if (!saveConfig(ctx.config, &saveErr)) {
    String message = saveErr.isEmpty() ? String("Failed to save config") : saveErr;
    message += " / previous config kept";
    ctx.ui->showToast("Save Error", message, 1900, backgroundTick);
    return;
  }

  ctx.configDirty = false;

  ctx.wifi->configure(ctx.config);
  ctx.gateway->configure(ctx.config);
  ctx.ble->configure(ctx.config);

  if (!ctx.config.gatewayUrl.isEmpty() && hasGatewayCredentials(ctx.config)) {
    ctx.gateway->reconnectNow();
  } else {
    ctx.gateway->disconnectNow();
  }

  if (ctx.config.bleDeviceAddress.isEmpty()) {
    ctx.ble->disconnectNow();
  } else if (ctx.config.bleAutoConnect) {
    String bleErr;
    if (!ctx.ble->connectToDevice(ctx.config.bleDeviceAddress,
                                  ctx.config.bleDeviceName,
                                  &bleErr)) {
      ctx.ui->showToast("BLE", bleErr, 1500, backgroundTick);
    }
  }

  ctx.ui->showToast("OpenClaw", "Saved and applied", 1400, backgroundTick);
}

std::vector<String> buildStatusLines(AppContext &ctx) {
  std::vector<String> lines;

  GatewayStatus gs = ctx.gateway->status();
  String cfgErr;
  const bool configOk = validateConfig(ctx.config, &cfgErr);

  lines.push_back("Config Valid: " + boolLabel(configOk));
  if (!configOk) {
    lines.push_back("OpenClaw settings required");
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
    if (ctx.configDirty) {
      subtitle += " *DIRTY";
    }

    std::vector<String> menu;
    menu.push_back("Status");
    menu.push_back("Gateway");
    menu.push_back("Save & Apply");
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

    if (choice < 0 || choice == 6) {
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
      runGatewayMenu(ctx, backgroundTick);
      continue;
    }

    if (choice == 2) {
      applyRuntimeConfig(ctx, backgroundTick);
      continue;
    }

    if (choice == 3) {
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

    if (choice == 4) {
      ctx.gateway->disconnectNow();
      ctx.ui->showToast("OpenClaw", "Disconnected", 1200, backgroundTick);
      continue;
    }

    if (choice == 5) {
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
