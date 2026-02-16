#include "settings_app.h"
#include "firmware_update_app.h"

#include <vector>

#include "../core/ble_manager.h"
#include "../core/gateway_client.h"
#include "../core/runtime_config.h"
#include "../core/wifi_manager.h"
#include "../ui/i18n.h"
#include "../ui/ui_runtime.h"

namespace {

void markDirty(AppContext &ctx) {
  ctx.configDirty = true;
}

bool saveSettingsConfig(AppContext &ctx,
                        const std::function<void()> &backgroundTick,
                        const char *toastTitle) {
  String validateErr;
  if (!validateConfig(ctx.config, &validateErr)) {
    if (validateErr.isEmpty()) {
      validateErr = "Config validation failed";
    }
    ctx.uiRuntime->showToast(toastTitle, validateErr, 1800, backgroundTick);
    return false;
  }

  String saveErr;
  if (!saveConfig(ctx.config, &saveErr)) {
    String message = saveErr.isEmpty() ? String("Failed to save config") : saveErr;
    message += " / previous config kept";
    ctx.uiRuntime->showToast("Save Error", message, 1900, backgroundTick);
    return false;
  }

  ctx.configDirty = false;
  return true;
}

void requestWifiReconnect(AppContext &ctx,
                          const std::function<void()> &backgroundTick,
                          bool showToast) {
  ctx.wifi->configure(ctx.config);
  if (ctx.config.wifiSsid.isEmpty()) {
    ctx.wifi->disconnect();
    if (showToast) {
      ctx.uiRuntime->showToast("Wi-Fi", "Wi-Fi disconnected", 1200, backgroundTick);
    }
    return;
  }

  ctx.wifi->connectNow();
  if (showToast) {
    ctx.uiRuntime->showToast("Wi-Fi",
                      "Connecting to " + ctx.config.wifiSsid,
                      1500,
                      backgroundTick);
  }
}

void editHiddenWifi(AppContext &ctx,
                    const std::function<void()> &backgroundTick) {
  String ssid = ctx.config.wifiSsid;
  String password = ctx.config.wifiPassword;

  if (!ctx.uiRuntime->textInput("Wi-Fi SSID", ssid, false, backgroundTick)) {
    return;
  }
  if (!ctx.uiRuntime->textInput("Wi-Fi Password", password, true, backgroundTick)) {
    return;
  }

  ctx.config.wifiSsid = ssid;
  ctx.config.wifiPassword = password;
  markDirty(ctx);
  requestWifiReconnect(ctx, backgroundTick, true);
  saveSettingsConfig(ctx, backgroundTick, "Wi-Fi");
}

void scanAndSelectWifi(AppContext &ctx,
                       const std::function<void()> &backgroundTick) {
  std::vector<String> ssids;
  String err;
  if (!ctx.wifi->scanNetworks(ssids, &err)) {
    String message = err.isEmpty() ? String("Wi-Fi scan failed") : err;
    message += " / use Hidden SSID";
    ctx.uiRuntime->showToast("Wi-Fi Scan", message, 1800, backgroundTick);
    return;
  }

  std::vector<String> menu = ssids;
  menu.push_back("Hidden SSID");
  menu.push_back("Back");

  int selected = 0;
  const int choice = ctx.uiRuntime->menuLoop("Wi-Fi Scan",
                                       menu,
                                       selected,
                                       backgroundTick,
                                       "OK Select  BACK Exit",
                                       "Pick SSID");

  if (choice < 0 || choice == static_cast<int>(menu.size()) - 1) {
    return;
  }

  if (menu[static_cast<size_t>(choice)] == "Hidden SSID") {
    editHiddenWifi(ctx, backgroundTick);
    return;
  }

  const String selectedSsid = menu[static_cast<size_t>(choice)];
  String password = ctx.config.wifiPassword;
  if (!ctx.uiRuntime->textInput("Wi-Fi Password", password, true, backgroundTick)) {
    return;
  }

  ctx.config.wifiSsid = selectedSsid;
  ctx.config.wifiPassword = password;
  markDirty(ctx);
  requestWifiReconnect(ctx, backgroundTick, true);
  saveSettingsConfig(ctx, backgroundTick, "Wi-Fi");
}

void runWifiMenu(AppContext &ctx,
                 const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    menu.push_back("Scan Networks");
    menu.push_back("Hidden SSID");
    menu.push_back("Connect Now");
    menu.push_back("Disconnect");
    menu.push_back("Clear Wi-Fi");
    menu.push_back("Back");

    const String subtitle = ctx.config.wifiSsid.isEmpty()
                                ? String("SSID: (empty)")
                                : String("SSID: ") + ctx.config.wifiSsid;

    const int choice = ctx.uiRuntime->menuLoop("Setting / Wi-Fi",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        subtitle);
    if (choice < 0 || choice == 5) {
      return;
    }
    selected = choice;

    if (choice == 0) {
      scanAndSelectWifi(ctx, backgroundTick);
    } else if (choice == 1) {
      editHiddenWifi(ctx, backgroundTick);
    } else if (choice == 2) {
      if (ctx.config.wifiSsid.isEmpty()) {
        ctx.uiRuntime->showToast("Wi-Fi", "SSID is empty", 1300, backgroundTick);
        continue;
      }
      requestWifiReconnect(ctx, backgroundTick, true);
    } else if (choice == 3) {
      ctx.wifi->disconnect();
      ctx.uiRuntime->showToast("Wi-Fi", "Disconnected", 1200, backgroundTick);
    } else if (choice == 4) {
      ctx.config.wifiSsid = "";
      ctx.config.wifiPassword = "";
      markDirty(ctx);
      requestWifiReconnect(ctx, backgroundTick, true);
      saveSettingsConfig(ctx, backgroundTick, "Wi-Fi");
    }
  }
}

String buildBleSubtitle(const AppContext &ctx) {
  const BleStatus bs = ctx.ble->status();
  String subtitle = bs.connected ? "Connected" : "Disconnected";

  if (!bs.profile.isEmpty()) {
    subtitle += " / " + bs.profile;
  }

  if (!bs.deviceName.isEmpty()) {
    subtitle += " / " + bs.deviceName;
  } else if (!bs.deviceAddress.isEmpty()) {
    subtitle += " / " + bs.deviceAddress;
  }

  return subtitle;
}

String keyboardPreview(const String &input) {
  if (input.isEmpty()) {
    return "(empty)";
  }

  String out;
  out.reserve(input.length() + 16);
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[static_cast<unsigned int>(i)];
    if (c == '\n') {
      out += "\\n";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c >= 32 && c <= 126) {
      out += c;
    } else {
      out += '.';
    }
  }

  constexpr size_t kMaxPreview = 80;
  if (out.length() > kMaxPreview) {
    out = out.substring(out.length() - kMaxPreview);
  }
  return out;
}

String displayBrightnessLabel(uint8_t percent) {
  String label = "Display Brightness: ";
  label += String(static_cast<unsigned long>(percent));
  label += "%";
  return label;
}

void showBleKeyboardInput(AppContext &ctx,
                          const std::function<void()> &backgroundTick) {
  const BleStatus bs = ctx.ble->status();

  std::vector<String> lines;
  lines.push_back("Connected: " + String(bs.connected ? "Yes" : "No"));
  lines.push_back("Profile: " + (bs.profile.isEmpty() ? String("(unknown)") : bs.profile));
  lines.push_back("HID: " + String(bs.hidDevice ? "Yes" : "No"));
  lines.push_back("Keyboard: " + String(bs.hidKeyboard ? "Yes" : "No"));
  lines.push_back("Input:");
  lines.push_back(keyboardPreview(bs.keyboardText));
  if (!bs.pairingHint.isEmpty()) {
    lines.push_back("Pairing:");
    lines.push_back(bs.pairingHint);
  }

  ctx.uiRuntime->showInfo("BLE Keyboard", lines, backgroundTick, "OK/BACK Exit");
}

void scanAndConnectBle(AppContext &ctx,
                       const std::function<void()> &backgroundTick) {
  std::vector<BleDeviceInfo> devices;
  String err;
  if (!ctx.ble->scanDevices(devices, &err)) {
    ctx.uiRuntime->showToast("BLE Scan",
                      err.isEmpty() ? String("BLE scan failed") : err,
                      1700,
                      backgroundTick);
    return;
  }

  if (devices.empty()) {
    ctx.uiRuntime->showToast("BLE Scan", "No BLE devices found", 1500, backgroundTick);
    return;
  }

  std::vector<String> menu;
  menu.reserve(devices.size() + 1);

  for (std::vector<BleDeviceInfo>::const_iterator it = devices.begin();
       it != devices.end();
       ++it) {
    String item;
    if (it->isKeyboard) {
      item = "[KBD] ";
    } else if (it->isLikelyAudio) {
      item = "[AUD] ";
    } else if (it->isHid) {
      item = "[HID] ";
    } else {
      item = "[BLE] ";
    }

    item += it->name;
    item += " (";
    item += String(it->rssi);
    item += " dBm)";
    menu.push_back(item);
  }
  menu.push_back("Back");

  int selected = 0;
  const int choice = ctx.uiRuntime->menuLoop("BLE Scan",
                                       menu,
                                       selected,
                                       backgroundTick,
                                       "OK Select  BACK Exit",
                                       "Pick BLE device");

  if (choice < 0 || choice == static_cast<int>(menu.size()) - 1) {
    return;
  }

  const BleDeviceInfo &device = devices[static_cast<size_t>(choice)];
  String connectErr;
  if (!ctx.ble->connectToDevice(device.address, device.name, &connectErr)) {
    ctx.uiRuntime->showToast("BLE Connect",
                      connectErr.isEmpty() ? String("BLE connect failed")
                                           : connectErr,
                      1800,
                      backgroundTick);
    return;
  }

  ctx.config.bleDeviceAddress = device.address;
  ctx.config.bleDeviceName = device.name;
  markDirty(ctx);

  const BleStatus status = ctx.ble->status();
  if (status.hidKeyboard) {
    ctx.uiRuntime->showToast("BLE", "Keyboard connected", 1400, backgroundTick);
  } else if (status.audioStreamAvailable) {
    ctx.uiRuntime->showToast("BLE", "Audio stream ready", 1500, backgroundTick);
  } else if (status.likelyAudio) {
    ctx.uiRuntime->showToast("BLE",
                      "Connected, but no audio stream characteristic",
                      1800,
                      backgroundTick);
  } else {
    ctx.uiRuntime->showToast("BLE", "Connected and staged", 1400, backgroundTick);
  }
}

void runBleMenu(AppContext &ctx,
                const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    menu.push_back("Scan & Connect");
    menu.push_back("Connect Saved");
    menu.push_back("Disconnect");
    menu.push_back("Keyboard Input View");
    menu.push_back("Clear Keyboard Input");
    menu.push_back("Edit Device Addr");
    menu.push_back("Edit Device Name");
    menu.push_back(String("Auto Connect: ") +
                   (ctx.config.bleAutoConnect ? "On" : "Off"));
    menu.push_back("Forget Saved");
    menu.push_back("Back");

    const int choice = ctx.uiRuntime->menuLoop("Setting / BLE",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        buildBleSubtitle(ctx));
    if (choice < 0 || choice == 9) {
      return;
    }
    selected = choice;

    if (choice == 0) {
      scanAndConnectBle(ctx, backgroundTick);
      continue;
    }

    if (choice == 1) {
      if (ctx.config.bleDeviceAddress.isEmpty()) {
        ctx.uiRuntime->showToast("BLE", "Saved address is empty", 1500, backgroundTick);
        continue;
      }

      String connectErr;
      if (!ctx.ble->connectToDevice(ctx.config.bleDeviceAddress,
                                    ctx.config.bleDeviceName,
                                    &connectErr)) {
        ctx.uiRuntime->showToast("BLE Connect",
                          connectErr.isEmpty() ? String("BLE connect failed")
                                               : connectErr,
                          1800,
                          backgroundTick);
      } else {
        ctx.uiRuntime->showToast("BLE", "Connected", 1200, backgroundTick);
      }
      continue;
    }

    if (choice == 2) {
      ctx.ble->disconnectNow();
      ctx.uiRuntime->showToast("BLE", "Disconnected", 1200, backgroundTick);
      continue;
    }

    if (choice == 3) {
      showBleKeyboardInput(ctx, backgroundTick);
      continue;
    }

    if (choice == 4) {
      ctx.ble->clearKeyboardInput();
      ctx.uiRuntime->showToast("BLE", "Keyboard input cleared", 1200, backgroundTick);
      continue;
    }

    if (choice == 5) {
      String address = ctx.config.bleDeviceAddress;
      if (ctx.uiRuntime->textInput("BLE Address", address, false, backgroundTick)) {
        address.toUpperCase();
        ctx.config.bleDeviceAddress = address;
        markDirty(ctx);
      }
      continue;
    }

    if (choice == 6) {
      String name = ctx.config.bleDeviceName;
      if (ctx.uiRuntime->textInput("BLE Name", name, false, backgroundTick)) {
        ctx.config.bleDeviceName = name;
        markDirty(ctx);
      }
      continue;
    }

    if (choice == 7) {
      ctx.config.bleAutoConnect = !ctx.config.bleAutoConnect;
      markDirty(ctx);
      ctx.uiRuntime->showToast("BLE",
                        ctx.config.bleAutoConnect ? "Auto connect enabled"
                                                  : "Auto connect disabled",
                        1300,
                        backgroundTick);
      continue;
    }

    if (choice == 8) {
      ctx.config.bleDeviceAddress = "";
      ctx.config.bleDeviceName = "";
      ctx.config.bleAutoConnect = false;
      ctx.ble->disconnectNow();
      markDirty(ctx);
      ctx.uiRuntime->showToast("BLE", "Saved BLE device cleared", 1400, backgroundTick);
      continue;
    }
  }
}

void runSystemMenu(AppContext &ctx,
                   const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    const UiLanguage currentLang = uiLanguageFromConfigCode(ctx.config.uiLanguage);
    String tzLabel = ctx.config.timezoneTz;
    tzLabel.trim();
    if (tzLabel.isEmpty()) {
      tzLabel = ctx.uiRuntime->timezone();
    }
    if (tzLabel.length() > 16) {
      tzLabel = tzLabel.substring(0, 13) + "...";
    }
    menu.push_back(String("UI Language: ") + uiLanguageLabel(currentLang));
    menu.push_back(displayBrightnessLabel(ctx.config.displayBrightnessPercent));
    menu.push_back(String("Timezone: ") + tzLabel);
    menu.push_back("Sync Timezone (IP)");
    menu.push_back("Factory Reset");
    menu.push_back("Back");

    const int choice = ctx.uiRuntime->menuLoop("Setting / System",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        "Runtime config control");
    if (choice < 0 || choice == 5) {
      return;
    }

    if (choice == 0) {
      std::vector<String> langItems;
      langItems.push_back("English");
      langItems.push_back("Korean");
      langItems.push_back("Back");

      const int langIndex = ctx.uiRuntime->menuLoop("UI Language",
                                                    langItems,
                                                    currentLang == UiLanguage::Korean ? 1 : 0,
                                                    backgroundTick,
                                                    "OK Select  BACK Exit",
                                                    "Language toggle");
      if (langIndex >= 0 && langIndex <= 1) {
        const UiLanguage nextLang = langIndex == 1 ? UiLanguage::Korean : UiLanguage::English;
        ctx.config.uiLanguage = uiLanguageCode(nextLang);
        ctx.uiRuntime->setLanguage(nextLang);
        markDirty(ctx);
        saveSettingsConfig(ctx, backgroundTick, "System");
      }
      continue;
    }

    if (choice == 1) {
      const uint8_t originalBrightness = ctx.config.displayBrightnessPercent;
      int brightnessPercent = static_cast<int>(originalBrightness);
      if (!ctx.uiRuntime->numberWheelInput("Brightness",
                                           0,
                                           100,
                                           1,
                                           brightnessPercent,
                                           backgroundTick,
                                           "%",
                                           [&](int value) {
                                             ctx.uiRuntime->setDisplayBrightnessPercent(
                                                 static_cast<uint8_t>(value));
                                           })) {
        ctx.uiRuntime->setDisplayBrightnessPercent(originalBrightness);
        continue;
      }

      ctx.config.displayBrightnessPercent = static_cast<uint8_t>(brightnessPercent);
      ctx.uiRuntime->setDisplayBrightnessPercent(ctx.config.displayBrightnessPercent);
      markDirty(ctx);
      saveSettingsConfig(ctx, backgroundTick, "System");
      continue;
    }

    if (choice == 2) {
      String tzInput = ctx.config.timezoneTz;
      tzInput.trim();
      if (tzInput.isEmpty()) {
        tzInput = ctx.uiRuntime->timezone();
      }

      if (!ctx.uiRuntime->textInput("Timezone TZ", tzInput, false, backgroundTick)) {
        continue;
      }

      tzInput.trim();
      if (tzInput.isEmpty()) {
        ctx.uiRuntime->showToast("System", "Timezone cannot be empty", 1400, backgroundTick);
        continue;
      }

      ctx.config.timezoneTz = tzInput;
      ctx.uiRuntime->setTimezone(tzInput);
      markDirty(ctx);
      saveSettingsConfig(ctx, backgroundTick, "System");
      continue;
    }

    if (choice == 3) {
      if (!ctx.wifi->isConnected()) {
        ctx.uiRuntime->showToast("System", "Wi-Fi required for IP timezone", 1600, backgroundTick);
        continue;
      }

      ctx.uiRuntime->showProgressOverlay("System", "Resolving timezone via IP...", -1);
      String resolvedTz;
      String syncErr;
      const bool synced = ctx.uiRuntime->syncTimezoneFromIp(&resolvedTz, &syncErr);
      ctx.uiRuntime->hideProgressOverlay();

      if (!synced) {
        if (syncErr.isEmpty()) {
          syncErr = "Failed to resolve timezone from IP";
        }
        ctx.uiRuntime->showToast("System", syncErr, 1700, backgroundTick);
        continue;
      }

      ctx.config.timezoneTz = resolvedTz;
      ctx.uiRuntime->setTimezone(resolvedTz);
      markDirty(ctx);
      if (saveSettingsConfig(ctx, backgroundTick, "System")) {
        ctx.uiRuntime->showToast("System",
                                 "Timezone synced: " + resolvedTz,
                                 1600,
                                 backgroundTick);
      }
      continue;
    }

    if (choice != 4) {
      continue;
    }

    if (!ctx.uiRuntime->confirm("Factory Reset",
                         "Delete Wi-Fi/Gateway config?",
                         backgroundTick,
                         "Yes",
                         "No")) {
      continue;
    }

    if (!ctx.uiRuntime->confirm("Confirm Again",
                         "This cannot be undone",
                         backgroundTick,
                         "Reset",
                         "Cancel")) {
      continue;
    }

    String resetErr;
    if (!resetConfig(&resetErr)) {
      ctx.uiRuntime->showToast("Reset Error", resetErr, 1600, backgroundTick);
      continue;
    }

    ctx.config = makeDefaultConfig();
    ctx.configDirty = false;

    ctx.wifi->configure(ctx.config);
    ctx.wifi->disconnect();

    ctx.gateway->disconnectNow();
    ctx.gateway->configure(ctx.config);

    ctx.ble->disconnectNow();
    ctx.ble->configure(ctx.config);

    ctx.uiRuntime->setLanguage(uiLanguageFromConfigCode(ctx.config.uiLanguage));
    ctx.uiRuntime->setTimezone(ctx.config.timezoneTz);
    ctx.uiRuntime->setDisplayBrightnessPercent(ctx.config.displayBrightnessPercent);
    ctx.uiRuntime->showToast("System", "Factory reset completed", 1600, backgroundTick);
    return;
  }
}

}  // namespace

void runSettingsApp(AppContext &ctx,
                    const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    menu.push_back("Wi-Fi");
    menu.push_back("BLE");
    menu.push_back("System");
    menu.push_back("Firmware Update");
    menu.push_back("Back");

    const String subtitle = ctx.configDirty ? "Unsaved changes" : "Saved";
    const int choice = ctx.uiRuntime->menuLoop("Setting",
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
      runWifiMenu(ctx, backgroundTick);
    } else if (choice == 1) {
      runBleMenu(ctx, backgroundTick);
    } else if (choice == 2) {
      runSystemMenu(ctx, backgroundTick);
    } else if (choice == 3) {
      runFirmwareUpdateApp(ctx, backgroundTick);
    }
  }
}
