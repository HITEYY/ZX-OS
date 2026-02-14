#include "settings_app.h"

#include <vector>

#include "../core/ble_manager.h"
#include "../core/gateway_client.h"
#include "../core/runtime_config.h"
#include "../core/wifi_manager.h"
#include "../ui/ui_shell.h"

namespace {

void markDirty(AppContext &ctx) {
  ctx.configDirty = true;
}

String buildBleSubtitle(const AppContext &ctx) {
  const BleStatus bs = ctx.ble->status();
  String subtitle = bs.connected ? "Connected" : "Disconnected";

  if (!bs.deviceName.isEmpty()) {
    subtitle += " / " + bs.deviceName;
  } else if (!bs.deviceAddress.isEmpty()) {
    subtitle += " / " + bs.deviceAddress;
  }

  return subtitle;
}

void scanAndConnectBle(AppContext &ctx,
                       const std::function<void()> &backgroundTick) {
  std::vector<BleDeviceInfo> devices;
  String err;
  if (!ctx.ble->scanDevices(devices, &err)) {
    ctx.ui->showToast("BLE Scan",
                      err.isEmpty() ? String("BLE scan failed") : err,
                      1700,
                      backgroundTick);
    return;
  }

  if (devices.empty()) {
    ctx.ui->showToast("BLE Scan", "No BLE devices found", 1500, backgroundTick);
    return;
  }

  std::vector<String> menu;
  menu.reserve(devices.size() + 1);

  for (std::vector<BleDeviceInfo>::const_iterator it = devices.begin();
       it != devices.end();
       ++it) {
    String item = it->name;
    item += " (";
    item += String(it->rssi);
    item += " dBm)";
    menu.push_back(item);
  }
  menu.push_back("Back");

  int selected = 0;
  const int choice = ctx.ui->menuLoop("BLE Scan",
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
    ctx.ui->showToast("BLE Connect",
                      connectErr.isEmpty() ? String("BLE connect failed")
                                           : connectErr,
                      1800,
                      backgroundTick);
    return;
  }

  ctx.config.bleDeviceAddress = device.address;
  ctx.config.bleDeviceName = device.name;
  markDirty(ctx);
  ctx.ui->showToast("BLE", "Connected and staged", 1400, backgroundTick);
}

void runBleMenu(AppContext &ctx,
                const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    menu.push_back("Scan & Connect");
    menu.push_back("Connect Saved");
    menu.push_back("Disconnect");
    menu.push_back("Edit Device Addr");
    menu.push_back("Edit Device Name");
    menu.push_back(String("Auto Connect: ") +
                   (ctx.config.bleAutoConnect ? "On" : "Off"));
    menu.push_back("Forget Saved");
    menu.push_back("Back");

    const int choice = ctx.ui->menuLoop("Setting / BLE",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        buildBleSubtitle(ctx));
    if (choice < 0 || choice == 7) {
      return;
    }
    selected = choice;

    if (choice == 0) {
      scanAndConnectBle(ctx, backgroundTick);
      continue;
    }

    if (choice == 1) {
      if (ctx.config.bleDeviceAddress.isEmpty()) {
        ctx.ui->showToast("BLE", "Saved address is empty", 1500, backgroundTick);
        continue;
      }

      String connectErr;
      if (!ctx.ble->connectToDevice(ctx.config.bleDeviceAddress,
                                    ctx.config.bleDeviceName,
                                    &connectErr)) {
        ctx.ui->showToast("BLE Connect",
                          connectErr.isEmpty() ? String("BLE connect failed")
                                               : connectErr,
                          1800,
                          backgroundTick);
      } else {
        ctx.ui->showToast("BLE", "Connected", 1200, backgroundTick);
      }
      continue;
    }

    if (choice == 2) {
      ctx.ble->disconnectNow();
      ctx.ui->showToast("BLE", "Disconnected", 1200, backgroundTick);
      continue;
    }

    if (choice == 3) {
      String address = ctx.config.bleDeviceAddress;
      if (ctx.ui->textInput("BLE Address", address, false, backgroundTick)) {
        address.toUpperCase();
        ctx.config.bleDeviceAddress = address;
        markDirty(ctx);
      }
      continue;
    }

    if (choice == 4) {
      String name = ctx.config.bleDeviceName;
      if (ctx.ui->textInput("BLE Name", name, false, backgroundTick)) {
        ctx.config.bleDeviceName = name;
        markDirty(ctx);
      }
      continue;
    }

    if (choice == 5) {
      ctx.config.bleAutoConnect = !ctx.config.bleAutoConnect;
      markDirty(ctx);
      ctx.ui->showToast("BLE",
                        ctx.config.bleAutoConnect ? "Auto connect enabled"
                                                  : "Auto connect disabled",
                        1300,
                        backgroundTick);
      continue;
    }

    if (choice == 6) {
      ctx.config.bleDeviceAddress = "";
      ctx.config.bleDeviceName = "";
      ctx.config.bleAutoConnect = false;
      ctx.ble->disconnectNow();
      markDirty(ctx);
      ctx.ui->showToast("BLE", "Saved BLE device cleared", 1400, backgroundTick);
      continue;
    }
  }
}

void runSystemMenu(AppContext &ctx,
                   const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    menu.push_back("Factory Reset");
    menu.push_back("Back");

    const int choice = ctx.ui->menuLoop("Setting / System",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        "Runtime config control");
    if (choice < 0 || choice == 1) {
      return;
    }

    if (!ctx.ui->confirm("Factory Reset",
                         "Delete Wi-Fi/Gateway config?",
                         backgroundTick,
                         "Yes",
                         "No")) {
      continue;
    }

    if (!ctx.ui->confirm("Confirm Again",
                         "This cannot be undone",
                         backgroundTick,
                         "Reset",
                         "Cancel")) {
      continue;
    }

    String resetErr;
    if (!resetConfig(&resetErr)) {
      ctx.ui->showToast("Reset Error", resetErr, 1600, backgroundTick);
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

    ctx.ui->showToast("System", "Factory reset completed", 1600, backgroundTick);
    return;
  }
}

}  // namespace

void runSettingsApp(AppContext &ctx,
                    const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    menu.push_back("BLE");
    menu.push_back("System");
    menu.push_back("Back");

    const String subtitle = ctx.configDirty ? "Unsaved changes" : "Saved";
    const int choice = ctx.ui->menuLoop("Setting",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        subtitle);

    if (choice < 0 || choice == 2) {
      return;
    }

    selected = choice;

    if (choice == 0) {
      runBleMenu(ctx, backgroundTick);
    } else if (choice == 1) {
      runSystemMenu(ctx, backgroundTick);
    }
  }
}
