#include "tailscale_app.h"

#include <WiFi.h>
#include <WiFiClient.h>

#include <vector>

#include "../core/ble_manager.h"
#include "../core/gateway_client.h"
#include "../core/runtime_config.h"
#include "../core/wifi_manager.h"
#include "../ui/ui_shell.h"

namespace {

struct RelayTarget {
  String host;
  uint16_t port = 18789;
  String path = "/";
  bool secure = false;
};

String boolLabel(bool value) {
  return value ? "Yes" : "No";
}

void markDirty(AppContext &ctx) {
  ctx.configDirty = true;
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

void normalizeTarget(RelayTarget &target) {
  if (target.port == 0) {
    target.port = 18789;
  }
  if (target.path.isEmpty()) {
    target.path = "/";
  }
  if (!target.path.startsWith("/")) {
    target.path = "/" + target.path;
  }
}

bool parseWsUrl(const String &rawUrl, RelayTarget &outTarget) {
  if (rawUrl.isEmpty()) {
    return false;
  }

  RelayTarget parsed;
  String rest;

  if (rawUrl.startsWith("ws://")) {
    parsed.secure = false;
    rest = rawUrl.substring(5);
  } else if (rawUrl.startsWith("wss://")) {
    parsed.secure = true;
    rest = rawUrl.substring(6);
  } else {
    return false;
  }

  const int slash = rest.indexOf('/');
  const String hostPort = slash >= 0 ? rest.substring(0, slash) : rest;
  parsed.path = slash >= 0 ? rest.substring(slash) : "/";

  if (hostPort.isEmpty()) {
    return false;
  }

  parsed.port = parsed.secure ? 443 : 80;

  if (hostPort.startsWith("[")) {
    const int close = hostPort.indexOf(']');
    if (close <= 1) {
      return false;
    }

    parsed.host = hostPort.substring(1, static_cast<unsigned int>(close));
    if (close + 1 < static_cast<int>(hostPort.length()) &&
        hostPort[static_cast<unsigned int>(close + 1)] == ':') {
      const String portText = hostPort.substring(static_cast<unsigned int>(close + 2));
      uint16_t parsedPort = 0;
      if (!parsePortNumber(portText, parsedPort)) {
        return false;
      }
      parsed.port = parsedPort;
    }
  } else {
    const int firstColon = hostPort.indexOf(':');
    const int lastColon = hostPort.lastIndexOf(':');

    if (firstColon > 0 && firstColon == lastColon) {
      parsed.host = hostPort.substring(0, static_cast<unsigned int>(firstColon));
      const String portText = hostPort.substring(static_cast<unsigned int>(firstColon + 1));
      uint16_t parsedPort = 0;
      if (!parsePortNumber(portText, parsedPort)) {
        return false;
      }
      parsed.port = parsedPort;
    } else {
      parsed.host = hostPort;
    }
  }

  if (parsed.host.isEmpty()) {
    return false;
  }

  normalizeTarget(parsed);
  outTarget = parsed;
  return true;
}

String buildRelayUrl(const RelayTarget &targetRaw) {
  RelayTarget target = targetRaw;
  normalizeTarget(target);

  String hostPart = target.host;
  if (hostPart.indexOf(':') >= 0 && !hostPart.startsWith("[")) {
    hostPart = "[" + hostPart + "]";
  }

  String url = target.secure ? "wss://" : "ws://";
  url += hostPart;
  url += ":";
  url += String(target.port);
  url += target.path;

  return url;
}

void applyRelayUrlToConfig(AppContext &ctx,
                           const RelayTarget &target,
                           const std::function<void()> &backgroundTick) {
  if (target.host.isEmpty()) {
    ctx.ui->showToast("Tailscale", "Relay host is empty", 1500, backgroundTick);
    return;
  }

  ctx.config.gatewayUrl = buildRelayUrl(target);
  markDirty(ctx);
  ctx.ui->showToast("Tailscale",
                    "Gateway URL staged",
                    1200,
                    backgroundTick);
}

void probeRelay(AppContext &ctx,
                const RelayTarget &target,
                String &lastProbeResult,
                const std::function<void()> &backgroundTick) {
  if (target.host.isEmpty()) {
    ctx.ui->showToast("Relay Probe", "Relay host is empty", 1500, backgroundTick);
    return;
  }

  if (!ctx.wifi->isConnected()) {
    ctx.ui->showToast("Relay Probe", "Wi-Fi is not connected", 1500, backgroundTick);
    return;
  }

  std::vector<String> lines;
  lines.push_back("Target: " + target.host + ":" + String(target.port));

  IPAddress resolved;
  if (WiFi.hostByName(target.host.c_str(), resolved) != 1) {
    lines.push_back("DNS: failed");
    lines.push_back("TCP: skipped");
    lastProbeResult = "DNS fail";
    ctx.ui->showInfo("Relay Probe", lines, backgroundTick, "OK/BACK Exit");
    return;
  }

  lines.push_back("DNS: " + resolved.toString());

  WiFiClient client;
  client.setTimeout(1500);
  const unsigned long startedAt = millis();
  const bool connected = client.connect(target.host.c_str(), target.port);
  const unsigned long elapsedMs = millis() - startedAt;

  if (connected) {
    lines.push_back("TCP: open");
    lines.push_back("Latency: " + String(elapsedMs) + " ms");
    lastProbeResult = "OK " + String(elapsedMs) + "ms";
    client.stop();
  } else {
    lines.push_back("TCP: closed / timeout");
    lines.push_back("Latency: " + String(elapsedMs) + " ms");
    lastProbeResult = "TCP fail";
  }

  ctx.ui->showInfo("Relay Probe", lines, backgroundTick, "OK/BACK Exit");
}

void showTailscaleStatus(AppContext &ctx,
                         const RelayTarget &target,
                         const String &lastProbeResult,
                         const std::function<void()> &backgroundTick) {
  const GatewayStatus gatewayStatus = ctx.gateway->status();

  std::vector<String> lines;
  lines.push_back("Tailscale in this firmware: Relay mode");
  lines.push_back("Wi-Fi Connected: " + boolLabel(ctx.wifi->isConnected()));
  lines.push_back("Wi-Fi SSID: " +
                  (ctx.wifi->ssid().isEmpty() ? String("(empty)") : ctx.wifi->ssid()));
  lines.push_back("Wi-Fi IP: " +
                  (ctx.wifi->ip().isEmpty() ? String("-") : ctx.wifi->ip()));

  if (target.host.isEmpty()) {
    lines.push_back("Relay Target: (not set)");
  } else {
    lines.push_back("Relay Target: " + target.host + ":" + String(target.port));
    lines.push_back("Relay URL: " + buildRelayUrl(target));
  }

  lines.push_back("Gateway URL: " +
                  (ctx.config.gatewayUrl.isEmpty() ? String("(empty)")
                                                   : ctx.config.gatewayUrl));
  lines.push_back("Auth Mode: " + String(gatewayAuthModeName(ctx.config.gatewayAuthMode)));
  lines.push_back("Credential Set: " + boolLabel(hasGatewayCredentials(ctx.config)));
  lines.push_back("Probe: " + lastProbeResult);
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

void editRelayHost(AppContext &ctx,
                   RelayTarget &target,
                   const std::function<void()> &backgroundTick) {
  String host = target.host;
  if (!ctx.ui->textInput("Relay Host/IP", host, false, backgroundTick)) {
    return;
  }

  host.trim();
  target.host = host;
  ctx.ui->showToast("Tailscale", "Relay host updated", 1200, backgroundTick);
}

void editRelayPort(AppContext &ctx,
                   RelayTarget &target,
                   const std::function<void()> &backgroundTick) {
  String portText = String(target.port);
  if (!ctx.ui->textInput("Relay Port", portText, false, backgroundTick)) {
    return;
  }

  uint16_t parsedPort = 0;
  if (!parsePortNumber(portText, parsedPort)) {
    ctx.ui->showToast("Tailscale", "Port must be 1..65535", 1500, backgroundTick);
    return;
  }

  target.port = parsedPort;
  ctx.ui->showToast("Tailscale", "Relay port updated", 1200, backgroundTick);
}

void editRelayPath(AppContext &ctx,
                   RelayTarget &target,
                   const std::function<void()> &backgroundTick) {
  String path = target.path;
  if (!ctx.ui->textInput("Relay Path", path, false, backgroundTick)) {
    return;
  }

  path.trim();
  target.path = path;
  normalizeTarget(target);
  ctx.ui->showToast("Tailscale", "Relay path updated", 1200, backgroundTick);
}

}  // namespace

void runTailscaleApp(AppContext &ctx,
                     const std::function<void()> &backgroundTick) {
  RelayTarget target;
  if (!parseWsUrl(ctx.config.gatewayUrl, target)) {
    target.host = "";
    target.port = 18789;
    target.path = "/";
    target.secure = false;
  }

  String lastProbeResult = "Not run";
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    menu.push_back("Status");
    menu.push_back("Relay Host/IP");
    menu.push_back("Relay Port");
    menu.push_back("Relay Path");
    menu.push_back(String("Scheme: ") + (target.secure ? "wss://" : "ws://"));
    menu.push_back("Apply URL to OpenClaw");
    menu.push_back("Probe Relay TCP");
    menu.push_back("Save & Apply");
    menu.push_back("Connect");
    menu.push_back("Disconnect");
    menu.push_back("Back");

    String subtitle;
    if (target.host.isEmpty()) {
      subtitle = "Set relay host first";
    } else {
      subtitle = trimMiddle(target.host, 18) + ":" + String(target.port);
    }

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
      showTailscaleStatus(ctx, target, lastProbeResult, backgroundTick);
      continue;
    }

    if (choice == 1) {
      editRelayHost(ctx, target, backgroundTick);
      continue;
    }

    if (choice == 2) {
      editRelayPort(ctx, target, backgroundTick);
      continue;
    }

    if (choice == 3) {
      editRelayPath(ctx, target, backgroundTick);
      continue;
    }

    if (choice == 4) {
      target.secure = !target.secure;
      ctx.ui->showToast("Tailscale",
                        target.secure ? "Scheme set to wss://" : "Scheme set to ws://",
                        1300,
                        backgroundTick);
      continue;
    }

    if (choice == 5) {
      applyRelayUrlToConfig(ctx, target, backgroundTick);
      continue;
    }

    if (choice == 6) {
      probeRelay(ctx, target, lastProbeResult, backgroundTick);
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
