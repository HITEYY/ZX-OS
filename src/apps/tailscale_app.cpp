#include "tailscale_app.h"

#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>

#include <algorithm>
#include <vector>

#include "../core/ble_manager.h"
#include "../core/board_pins.h"
#include "../core/gateway_client.h"
#include "../core/runtime_config.h"
#include "../core/tailscale_lite_client.h"
#include "../core/wifi_manager.h"
#include "../ui/ui_shell.h"

namespace {

struct EnvFileEntry {
  String fullPath;
  String label;
  bool isDirectory = false;
};

struct LiteEnvProfile {
  String authKey;
  String loginServer;
  String nodeIp;
  String privateKey;
  String peerHost;
  uint16_t peerPort = 41641;
  String peerPublicKey;
  String gatewayUrl;
};

bool gSdMountedForTailscale = false;

String boolLabel(bool value) {
  return value ? "Yes" : "No";
}

void markDirty(AppContext &ctx) {
  ctx.configDirty = true;
}

String baseName(const String &path) {
  const int slash = path.lastIndexOf('/');
  if (slash < 0 || slash + 1 >= static_cast<int>(path.length())) {
    return path;
  }
  return path.substring(static_cast<unsigned int>(slash + 1));
}

String parentPath(const String &path) {
  if (path.isEmpty() || path == "/") {
    return "/";
  }
  const int slash = path.lastIndexOf('/');
  if (slash <= 0) {
    return "/";
  }
  return path.substring(0, static_cast<unsigned int>(slash));
}

String buildChildPath(const String &dirPath, const String &name) {
  if (name.startsWith("/")) {
    return name;
  }
  if (dirPath == "/") {
    return "/" + name;
  }
  return dirPath + "/" + name;
}

String trimMiddle(const String &value, size_t maxLength) {
  if (value.length() <= maxLength || maxLength < 6) {
    return value;
  }

  const size_t left = (maxLength - 3) / 2;
  const size_t right = maxLength - 3 - left;
  return value.substring(0, left) + "..." +
         value.substring(value.length() - right);
}

bool parsePortNumber(const String &text, uint16_t &outPort) {
  if (text.isEmpty()) {
    return false;
  }

  const long value = text.toInt();
  if (value < 1 || value > 65535) {
    return false;
  }

  outPort = static_cast<uint16_t>(value);
  return true;
}

bool ensureSdMountedForTailscale(bool forceMount, String *error) {
  if (gSdMountedForTailscale && !forceMount) {
    return true;
  }

  pinMode(boardpins::kTftCs, OUTPUT);
  digitalWrite(boardpins::kTftCs, HIGH);
  pinMode(boardpins::kCc1101Cs, OUTPUT);
  digitalWrite(boardpins::kCc1101Cs, HIGH);
  pinMode(boardpins::kSdCs, OUTPUT);
  digitalWrite(boardpins::kSdCs, HIGH);

  SPIClass *spiBus = &TFT_eSPI::getSPIinstance();
  const bool mounted = SD.begin(boardpins::kSdCs,
                                *spiBus,
                                25000000,
                                "/sd",
                                8,
                                false);
  gSdMountedForTailscale = mounted;
  if (!mounted && error) {
    *error = "SD mount failed";
  }
  return mounted;
}

bool isEnvFileName(const String &nameRaw) {
  String name = nameRaw;
  name.toLowerCase();
  return name == ".env" || name.endsWith(".env");
}

bool listEnvDirectory(const String &path,
                      std::vector<EnvFileEntry> &outEntries,
                      String *error) {
  outEntries.clear();

  File dir = SD.open(path.c_str(), FILE_READ);
  if (!dir || !dir.isDirectory()) {
    if (error) {
      *error = "Directory open failed";
    }
    if (dir) {
      dir.close();
    }
    return false;
  }

  File entry = dir.openNextFile();
  while (entry) {
    const String rawName = String(entry.name());
    if (!rawName.isEmpty()) {
      const bool isDir = entry.isDirectory();
      const String name = baseName(buildChildPath(path, rawName));

      if (isDir || isEnvFileName(name)) {
        EnvFileEntry item;
        item.fullPath = buildChildPath(path, rawName);
        item.isDirectory = isDir;
        item.label = isDir ? "[D] " : "[ENV] ";
        item.label += name;
        outEntries.push_back(item);
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

  std::sort(outEntries.begin(),
            outEntries.end(),
            [](const EnvFileEntry &a, const EnvFileEntry &b) {
              if (a.isDirectory != b.isDirectory) {
                return a.isDirectory;
              }
              String lhs = a.fullPath;
              String rhs = b.fullPath;
              lhs.toLowerCase();
              rhs.toLowerCase();
              return lhs < rhs;
            });
  return true;
}

bool selectEnvFileFromSd(AppContext &ctx,
                         String &selectedPathOut,
                         const std::function<void()> &backgroundTick) {
  String err;
  if (!ensureSdMountedForTailscale(false, &err)) {
    ctx.ui->showToast("SD Card",
                      err.isEmpty() ? String("Mount failed") : err,
                      1700,
                      backgroundTick);
    return false;
  }

  String currentPath = "/";
  int selected = 0;

  while (true) {
    std::vector<EnvFileEntry> entries;
    if (!listEnvDirectory(currentPath, entries, &err)) {
      ctx.ui->showToast("Env Select",
                        err.isEmpty() ? String("Read failed") : err,
                        1700,
                        backgroundTick);
      return false;
    }

    std::vector<String> menu;
    if (currentPath != "/") {
      menu.push_back(".. (Up)");
    }
    for (std::vector<EnvFileEntry>::const_iterator it = entries.begin();
         it != entries.end();
         ++it) {
      menu.push_back(it->label);
    }
    menu.push_back("Refresh");
    menu.push_back("Back");

    const String subtitle = "Path: " + trimMiddle(currentPath, 22);
    const int choice = ctx.ui->menuLoop("Select .env",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        subtitle);
    if (choice < 0) {
      return false;
    }
    selected = choice;

    int idx = choice;
    if (currentPath != "/") {
      if (idx == 0) {
        currentPath = parentPath(currentPath);
        selected = 0;
        continue;
      }
      idx -= 1;
    }

    const int entryCount = static_cast<int>(entries.size());
    if (idx == entryCount) {
      continue;
    }
    if (idx == entryCount + 1) {
      return false;
    }
    if (idx < 0 || idx >= entryCount) {
      continue;
    }

    const EnvFileEntry picked = entries[static_cast<size_t>(idx)];
    if (picked.isDirectory) {
      currentPath = picked.fullPath;
      selected = 0;
      continue;
    }

    selectedPathOut = picked.fullPath;
    return true;
  }
}

String parseEnvValue(const String &lineIn) {
  String line = lineIn;
  line.trim();
  if (line.startsWith("\"") && line.endsWith("\"") && line.length() >= 2) {
    return line.substring(1, line.length() - 1);
  }
  if (line.startsWith("'") && line.endsWith("'") && line.length() >= 2) {
    return line.substring(1, line.length() - 1);
  }
  return line;
}

bool parseEnvFileForAuth(const String &path,
                         String &authKeyOut,
                         String &loginServerOut,
                         String *error) {
  authKeyOut = "";
  loginServerOut = "";

  File file = SD.open(path.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    if (error) {
      *error = "Failed to open .env";
    }
    return false;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.replace("\r", "");
    line.trim();
    if (line.isEmpty() || line.startsWith("#")) {
      continue;
    }
    if (line.startsWith("export ")) {
      line.remove(0, 7);
      line.trim();
    }

    const int eq = line.indexOf('=');
    if (eq <= 0) {
      continue;
    }

    String key = line.substring(0, static_cast<unsigned int>(eq));
    String value = line.substring(static_cast<unsigned int>(eq + 1));
    key.trim();
    value = parseEnvValue(value);

    if (key == "TAILSCALE_AUTH_KEY" ||
        key == "TAILSCALE_AUTHKEY" ||
        key == "TS_AUTHKEY" ||
        key == "tailscale_auth_key" ||
        key == "tailscale_authkey") {
      authKeyOut = value;
    } else if (key == "TAILSCALE_LOGIN_SERVER" ||
               key == "HEADSCALE_URL" ||
               key == "tailscale_login_server" ||
               key == "headscale_url") {
      loginServerOut = value;
    }
  }

  file.close();

  if (authKeyOut.isEmpty()) {
    if (error) {
      *error = "No auth key in .env";
    }
    return false;
  }
  return true;
}

bool parseEnvFileForLite(const String &path,
                         LiteEnvProfile &profileOut,
                         String *error) {
  profileOut = LiteEnvProfile();

  File file = SD.open(path.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    if (error) {
      *error = "Failed to open .env";
    }
    return false;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.replace("\r", "");
    line.trim();
    if (line.isEmpty() || line.startsWith("#")) {
      continue;
    }
    if (line.startsWith("export ")) {
      line.remove(0, 7);
      line.trim();
    }

    const int eq = line.indexOf('=');
    if (eq <= 0) {
      continue;
    }

    String key = line.substring(0, static_cast<unsigned int>(eq));
    String value = line.substring(static_cast<unsigned int>(eq + 1));
    key.trim();
    value = parseEnvValue(value);

    if (key == "TAILSCALE_AUTH_KEY" ||
        key == "TAILSCALE_AUTHKEY" ||
        key == "TS_AUTHKEY" ||
        key == "tailscale_auth_key" ||
        key == "tailscale_authkey") {
      profileOut.authKey = value;
    } else if (key == "TAILSCALE_LOGIN_SERVER" ||
               key == "HEADSCALE_URL" ||
               key == "tailscale_login_server" ||
               key == "headscale_url") {
      profileOut.loginServer = value;
    } else if (key == "TAILSCALE_LITE_NODE_IP" ||
               key == "TS_LITE_NODE_IP" ||
               key == "TS_WG_LOCAL_IP" ||
               key == "tailscale_lite_node_ip" ||
               key == "ts_lite_node_ip") {
      profileOut.nodeIp = value;
    } else if (key == "TAILSCALE_LITE_PRIVATE_KEY" ||
               key == "TS_LITE_PRIVATE_KEY" ||
               key == "TS_WG_PRIVATE_KEY" ||
               key == "tailscale_lite_private_key" ||
               key == "ts_lite_private_key") {
      profileOut.privateKey = value;
    } else if (key == "TAILSCALE_LITE_PEER_HOST" ||
               key == "TS_LITE_PEER_HOST" ||
               key == "TS_WG_ENDPOINT" ||
               key == "tailscale_lite_peer_host" ||
               key == "ts_lite_peer_host") {
      profileOut.peerHost = value;
    } else if (key == "TAILSCALE_LITE_PEER_PORT" ||
               key == "TS_LITE_PEER_PORT" ||
               key == "TS_WG_ENDPOINT_PORT" ||
               key == "tailscale_lite_peer_port" ||
               key == "ts_lite_peer_port") {
      uint16_t parsedPort = 0;
      if (parsePortNumber(value, parsedPort)) {
        profileOut.peerPort = parsedPort;
      }
    } else if (key == "TAILSCALE_LITE_PEER_PUBLIC_KEY" ||
               key == "TS_LITE_PEER_PUBLIC_KEY" ||
               key == "TS_WG_PEER_PUBLIC_KEY" ||
               key == "tailscale_lite_peer_public_key" ||
               key == "ts_lite_peer_public_key") {
      profileOut.peerPublicKey = value;
    } else if (key == "OPENCLAW_GATEWAY_URL" ||
               key == "GATEWAY_URL" ||
               key == "openclaw_gateway_url") {
      profileOut.gatewayUrl = value;
    }
  }

  file.close();

  if (profileOut.nodeIp.isEmpty() ||
      profileOut.privateKey.isEmpty() ||
      profileOut.peerHost.isEmpty() ||
      profileOut.peerPublicKey.isEmpty()) {
    if (error) {
      *error = "No lite tunnel profile in .env";
    }
    return false;
  }

  return true;
}

bool hasLiteProfileConfig(const RuntimeConfig &config) {
  return !config.tailscaleLiteNodeIp.isEmpty() &&
         !config.tailscaleLitePrivateKey.isEmpty() &&
         !config.tailscaleLitePeerHost.isEmpty() &&
         !config.tailscaleLitePeerPublicKey.isEmpty() &&
         config.tailscaleLitePeerPort > 0;
}

void showTailscaleStatus(AppContext &ctx,
                         const String &lastAuthLoadResult,
                         const String &lastLiteSetupResult,
                         const std::function<void()> &backgroundTick) {
  const GatewayStatus gatewayStatus = ctx.gateway->status();
  const TailscaleLiteStatus liteStatus =
      ctx.tailscaleLite ? ctx.tailscaleLite->status() : TailscaleLiteStatus();

  std::vector<String> lines;
  lines.push_back("Tailscale mode: Lite direct");
  lines.push_back("Wi-Fi Connected: " + boolLabel(ctx.wifi->isConnected()));
  lines.push_back("Wi-Fi SSID: " +
                  (ctx.wifi->ssid().isEmpty() ? String("(empty)") : ctx.wifi->ssid()));
  lines.push_back("Wi-Fi IP: " +
                  (ctx.wifi->ip().isEmpty() ? String("-") : ctx.wifi->ip()));
  lines.push_back("Gateway URL: " +
                  (ctx.config.gatewayUrl.isEmpty() ? String("(empty)")
                                                   : ctx.config.gatewayUrl));
  lines.push_back("Auth Mode: " + String(gatewayAuthModeName(ctx.config.gatewayAuthMode)));
  lines.push_back("Credential Set: " + boolLabel(hasGatewayCredentials(ctx.config)));

  lines.push_back("Login Server: " +
                  (ctx.config.tailscaleLoginServer.isEmpty()
                       ? String("(default tailscale)")
                       : trimMiddle(ctx.config.tailscaleLoginServer, 26)));
  lines.push_back("Auth Key Set: " + boolLabel(!ctx.config.tailscaleAuthKey.isEmpty()));
  lines.push_back("Auth .env Load: " + lastAuthLoadResult);
  lines.push_back("Lite Setup: " + lastLiteSetupResult);
  lines.push_back("Lite Profile Ready: " + boolLabel(hasLiteProfileConfig(ctx.config)));

  lines.push_back("Lite Enabled: " + boolLabel(liteStatus.enabled));
  lines.push_back("Lite Tunnel: " + boolLabel(liteStatus.tunnelUp));
  lines.push_back("Lite Node IP: " +
                  (ctx.config.tailscaleLiteNodeIp.isEmpty()
                       ? String("(empty)")
                       : ctx.config.tailscaleLiteNodeIp));
  lines.push_back("Lite Peer: " +
                  (ctx.config.tailscaleLitePeerHost.isEmpty()
                       ? String("(empty)")
                       : ctx.config.tailscaleLitePeerHost + ":" +
                             String(ctx.config.tailscaleLitePeerPort)));
  lines.push_back("Lite Peer Key: " +
                  boolLabel(!ctx.config.tailscaleLitePeerPublicKey.isEmpty()));
  lines.push_back("Lite Error: " +
                  (liteStatus.lastError.isEmpty() ? String("-") : liteStatus.lastError));

  lines.push_back("WS Connected: " + boolLabel(gatewayStatus.wsConnected));
  lines.push_back("Gateway Ready: " + boolLabel(gatewayStatus.gatewayReady));

  if (!gatewayStatus.lastError.isEmpty()) {
    lines.push_back("Last Error: " + gatewayStatus.lastError);
  }

  ctx.ui->showInfo("Tailscale Status", lines, backgroundTick, "OK/BACK Exit");
}

void saveAndApply(AppContext &ctx,
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
  if (ctx.tailscaleLite) {
    ctx.tailscaleLite->configure(ctx.config);
    if (ctx.config.tailscaleLiteEnabled) {
      String liteErr;
      if (!ctx.tailscaleLite->connectNow(&liteErr) && !liteErr.isEmpty()) {
        ctx.ui->showToast("Tailscale Lite", liteErr, 1600, backgroundTick);
      }
    } else {
      ctx.tailscaleLite->disconnectNow();
    }
  }

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

  ctx.ui->showToast("Tailscale", "Saved and applied", 1400, backgroundTick);
}

void requestGatewayConnect(AppContext &ctx,
                           const std::function<void()> &backgroundTick) {
  String validateErr;
  if (!validateConfig(ctx.config, &validateErr)) {
    ctx.ui->showToast("Config Error", validateErr, 1800, backgroundTick);
    return;
  }

  if (ctx.config.gatewayUrl.isEmpty()) {
    ctx.ui->showToast("Config Error", "Set gateway URL first", 1600, backgroundTick);
    return;
  }

  ctx.gateway->configure(ctx.config);
  ctx.gateway->connectNow();
  ctx.ui->showToast("Tailscale", "Connect requested", 1200, backgroundTick);
}

void editAuthKey(AppContext &ctx,
                 const std::function<void()> &backgroundTick) {
  String value = ctx.config.tailscaleAuthKey;
  if (!ctx.ui->textInput("Tailscale Auth Key", value, true, backgroundTick)) {
    return;
  }

  value.trim();
  ctx.config.tailscaleAuthKey = value;
  markDirty(ctx);
  ctx.ui->showToast("Tailscale", "Auth key updated", 1200, backgroundTick);
}

void runAuthLoadFromEnvFile(AppContext &ctx,
                            String &lastAuthLoadResult,
                            const std::function<void()> &backgroundTick) {
  String envPath;
  if (!selectEnvFileFromSd(ctx, envPath, backgroundTick)) {
    return;
  }

  String authKey;
  String loginServer;
  String err;
  if (!parseEnvFileForAuth(envPath, authKey, loginServer, &err)) {
    lastAuthLoadResult = err;
    ctx.ui->showToast("Tailscale .env", err, 1800, backgroundTick);
    return;
  }

  ctx.config.tailscaleAuthKey = authKey;
  if (!loginServer.isEmpty()) {
    ctx.config.tailscaleLoginServer = loginServer;
  }
  markDirty(ctx);

  String message = "Auth key loaded";
  if (!loginServer.isEmpty()) {
    message += " + login server";
  }
  ctx.ui->showToast("Tailscale .env", message, 1500, backgroundTick);
  lastAuthLoadResult = "Loaded";
}

void toggleLiteEnabled(AppContext &ctx,
                       const std::function<void()> &backgroundTick) {
  ctx.config.tailscaleLiteEnabled = !ctx.config.tailscaleLiteEnabled;
  markDirty(ctx);
  ctx.ui->showToast("Tailscale Lite",
                    ctx.config.tailscaleLiteEnabled ? "Enabled" : "Disabled",
                    1200,
                    backgroundTick);
}

void runLiteQuickSetupFromEnvFile(AppContext &ctx,
                                  String &lastLiteSetupResult,
                                  const std::function<void()> &backgroundTick) {
  String envPath;
  if (!selectEnvFileFromSd(ctx, envPath, backgroundTick)) {
    return;
  }

  LiteEnvProfile profile;
  String err;
  if (!parseEnvFileForLite(envPath, profile, &err)) {
    lastLiteSetupResult = err.isEmpty() ? String("Lite profile load failed") : err;
    ctx.ui->showToast("Tailscale Lite", err, 1800, backgroundTick);
    return;
  }

  ctx.config.tailscaleLiteEnabled = true;
  ctx.config.tailscaleLiteNodeIp = profile.nodeIp;
  ctx.config.tailscaleLitePrivateKey = profile.privateKey;
  ctx.config.tailscaleLitePeerHost = profile.peerHost;
  ctx.config.tailscaleLitePeerPort = profile.peerPort;
  ctx.config.tailscaleLitePeerPublicKey = profile.peerPublicKey;

  if (!profile.authKey.isEmpty()) {
    ctx.config.tailscaleAuthKey = profile.authKey;
  }
  if (!profile.loginServer.isEmpty()) {
    ctx.config.tailscaleLoginServer = profile.loginServer;
  }

  bool gatewayApplied = false;
  if (!profile.gatewayUrl.isEmpty() && hasGatewayCredentials(ctx.config)) {
    ctx.config.gatewayUrl = profile.gatewayUrl;
    gatewayApplied = true;
  }

  if (ctx.config.tailscaleAuthKey.isEmpty()) {
    ctx.ui->showToast("Tailscale Lite",
                      "Auth key required (set Auth or Auth from .env)",
                      1900,
                      backgroundTick);
    lastLiteSetupResult = "Auth key missing";
    return;
  }

  markDirty(ctx);
  saveAndApply(ctx, backgroundTick);

  if (ctx.configDirty) {
    lastLiteSetupResult = "Save/apply failed";
    return;
  }

  String message = "Lite setup applied";
  if (!profile.authKey.isEmpty()) {
    message += " + auth key";
  }
  if (gatewayApplied) {
    message += " + gateway URL";
  } else if (!profile.gatewayUrl.isEmpty()) {
    message += " (gateway skipped)";
  }
  ctx.ui->showToast("Tailscale Lite", message, 1600, backgroundTick);
  lastLiteSetupResult = "Applied";
}

void runLiteConnect(AppContext &ctx,
                    const std::function<void()> &backgroundTick) {
  if (!ctx.tailscaleLite) {
    ctx.ui->showToast("Tailscale Lite", "Lite client unavailable", 1500, backgroundTick);
    return;
  }

  String validateErr;
  if (!validateConfig(ctx.config, &validateErr)) {
    ctx.ui->showToast("Validation", validateErr, 1800, backgroundTick);
    return;
  }

  ctx.tailscaleLite->configure(ctx.config);
  String err;
  if (!ctx.tailscaleLite->connectNow(&err)) {
    ctx.ui->showToast("Tailscale Lite",
                      err.isEmpty() ? String("Connect failed") : err,
                      1800,
                      backgroundTick);
    return;
  }

  ctx.ui->showToast("Tailscale Lite", "Tunnel connected", 1200, backgroundTick);
}

void runLiteDisconnect(AppContext &ctx,
                       const std::function<void()> &backgroundTick) {
  if (!ctx.tailscaleLite) {
    ctx.ui->showToast("Tailscale Lite", "Lite client unavailable", 1500, backgroundTick);
    return;
  }

  ctx.tailscaleLite->disconnectNow();
  ctx.ui->showToast("Tailscale Lite", "Tunnel disconnected", 1200, backgroundTick);
}

}  // namespace

void runTailscaleApp(AppContext &ctx,
                     const std::function<void()> &backgroundTick) {
  if (ctx.config.tailscaleLitePeerPort == 0) {
    ctx.config.tailscaleLitePeerPort = 41641;
  }

  String lastAuthLoadResult = "Not run";
  String lastLiteSetupResult = "Not run";
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    menu.push_back("Status");
    menu.push_back("Auth Key");
    menu.push_back("Auth Load from SD .env");
    menu.push_back("Lite Quick Setup from SD .env");
    menu.push_back(String("Lite Enabled: ") +
                   (ctx.config.tailscaleLiteEnabled ? "Yes" : "No"));
    menu.push_back("Lite Connect");
    menu.push_back("Lite Disconnect");
    menu.push_back("Save & Apply");
    menu.push_back("Connect");
    menu.push_back("Disconnect");
    menu.push_back("Back");

    String subtitle = "Lite:";
    subtitle += (ctx.tailscaleLite && ctx.tailscaleLite->isConnected())
                    ? "UP"
                    : (ctx.config.tailscaleLiteEnabled ? "CFG" : "OFF");
    subtitle += " / Auth:";
    subtitle += ctx.config.tailscaleAuthKey.isEmpty() ? "EMPTY" : "SET";

    if (ctx.configDirty) {
      subtitle += " *DIRTY";
    }

    const int choice = ctx.ui->menuLoop("Tailscale",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        subtitle);

    if (choice < 0 || choice == 10) {
      return;
    }

    selected = choice;

    if (choice == 0) {
      showTailscaleStatus(ctx, lastAuthLoadResult, lastLiteSetupResult, backgroundTick);
      continue;
    }
    if (choice == 1) {
      editAuthKey(ctx, backgroundTick);
      continue;
    }
    if (choice == 2) {
      runAuthLoadFromEnvFile(ctx, lastAuthLoadResult, backgroundTick);
      continue;
    }
    if (choice == 3) {
      runLiteQuickSetupFromEnvFile(ctx, lastLiteSetupResult, backgroundTick);
      continue;
    }
    if (choice == 4) {
      toggleLiteEnabled(ctx, backgroundTick);
      continue;
    }
    if (choice == 5) {
      runLiteConnect(ctx, backgroundTick);
      continue;
    }
    if (choice == 6) {
      runLiteDisconnect(ctx, backgroundTick);
      continue;
    }
    if (choice == 7) {
      saveAndApply(ctx, backgroundTick);
      continue;
    }
    if (choice == 8) {
      requestGatewayConnect(ctx, backgroundTick);
      continue;
    }
    if (choice == 9) {
      ctx.gateway->disconnectNow();
      ctx.ui->showToast("Tailscale", "Disconnected", 1200, backgroundTick);
      continue;
    }
  }
}
