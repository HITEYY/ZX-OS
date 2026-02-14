#include "runtime_config.h"

#include <ArduinoJson.h>
#include <Preferences.h>

#include "user_config.h"

namespace {

constexpr const char *kPrefsNamespace = "oc_cfg";
constexpr const char *kConfigVersionKey = "cfg_ver";
constexpr const char *kConfigBlobKey = "cfg_blob";
constexpr uint32_t kConfigVersion = 2;

bool isPlaceholder(const char *value) {
  if (!value || !value[0]) {
    return true;
  }
  return strncmp(value, "REPLACE_WITH_", 13) == 0;
}

bool startsWithWsScheme(const String &url) {
  return url.startsWith("ws://") || url.startsWith("wss://");
}

bool isHexDigitChar(char c) {
  return (c >= '0' && c <= '9') ||
         (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

bool isValidBleAddress(const String &address) {
  if (address.isEmpty()) {
    return true;
  }
  if (address.length() != 17) {
    return false;
  }

  for (int i = 0; i < 17; ++i) {
    const char c = address[static_cast<unsigned int>(i)];
    if (i == 2 || i == 5 || i == 8 || i == 11 || i == 14) {
      if (c != ':') {
        return false;
      }
    } else if (!isHexDigitChar(c)) {
      return false;
    }
  }
  return true;
}

GatewayAuthMode sanitizeAuthMode(int mode) {
  return mode == 1 ? GatewayAuthMode::Password : GatewayAuthMode::Token;
}

void toJson(const RuntimeConfig &config, JsonObject obj) {
  obj["version"] = config.version;
  obj["wifiSsid"] = config.wifiSsid;
  obj["wifiPassword"] = config.wifiPassword;
  obj["gatewayUrl"] = config.gatewayUrl;
  obj["gatewayAuthMode"] = static_cast<uint8_t>(config.gatewayAuthMode);
  obj["gatewayToken"] = config.gatewayToken;
  obj["gatewayPassword"] = config.gatewayPassword;
  obj["autoConnect"] = config.autoConnect;
  obj["bleDeviceName"] = config.bleDeviceName;
  obj["bleDeviceAddress"] = config.bleDeviceAddress;
  obj["bleAutoConnect"] = config.bleAutoConnect;
}

void fromJson(const JsonObjectConst &obj, RuntimeConfig &config) {
  config.version = obj["version"] | kConfigVersion;
  config.wifiSsid = String(static_cast<const char *>(obj["wifiSsid"] | ""));
  config.wifiPassword = String(static_cast<const char *>(obj["wifiPassword"] | ""));
  config.gatewayUrl = String(static_cast<const char *>(obj["gatewayUrl"] | ""));
  config.gatewayAuthMode = sanitizeAuthMode(obj["gatewayAuthMode"] | 0);
  config.gatewayToken = String(static_cast<const char *>(obj["gatewayToken"] | ""));
  config.gatewayPassword = String(static_cast<const char *>(obj["gatewayPassword"] | ""));
  config.autoConnect = obj["autoConnect"] | false;
  config.bleDeviceName = String(static_cast<const char *>(obj["bleDeviceName"] | ""));
  config.bleDeviceAddress = String(static_cast<const char *>(obj["bleDeviceAddress"] | ""));
  config.bleAutoConnect = obj["bleAutoConnect"] | false;
}

}  // namespace

RuntimeConfig makeDefaultConfig() {
  RuntimeConfig config;
  config.version = kConfigVersion;

  if (!isPlaceholder(USER_WIFI_SSID)) {
    config.wifiSsid = USER_WIFI_SSID;
  }
  if (!isPlaceholder(USER_WIFI_PASSWORD)) {
    config.wifiPassword = USER_WIFI_PASSWORD;
  }

  if (!isPlaceholder(USER_GATEWAY_URL)) {
    config.gatewayUrl = USER_GATEWAY_URL;
  }

  config.gatewayAuthMode = sanitizeAuthMode(USER_GATEWAY_AUTH_MODE);
  if (!isPlaceholder(USER_GATEWAY_TOKEN)) {
    config.gatewayToken = USER_GATEWAY_TOKEN;
  }
  if (!isPlaceholder(USER_GATEWAY_PASSWORD)) {
    config.gatewayPassword = USER_GATEWAY_PASSWORD;
  }

  if (config.gatewayToken.isEmpty() && !config.gatewayPassword.isEmpty()) {
    config.gatewayAuthMode = GatewayAuthMode::Password;
  }

  config.autoConnect = USER_AUTO_CONNECT_DEFAULT;
  return config;
}

bool hasGatewayCredentials(const RuntimeConfig &config) {
  if (config.gatewayAuthMode == GatewayAuthMode::Token) {
    return config.gatewayToken.length() > 0;
  }
  return config.gatewayPassword.length() > 0;
}

bool validateConfig(const RuntimeConfig &config, String *error) {
  if (config.wifiSsid.isEmpty() && !config.wifiPassword.isEmpty()) {
    if (error) {
      *error = "Wi-Fi password exists but SSID is empty";
    }
    return false;
  }

  if (!config.gatewayUrl.isEmpty()) {
    if (!startsWithWsScheme(config.gatewayUrl)) {
      if (error) {
        *error = "Gateway URL must start with ws:// or wss://";
      }
      return false;
    }

    if (!hasGatewayCredentials(config)) {
      if (error) {
        *error = "Gateway auth credential is missing";
      }
      return false;
    }
  }

  if (!isValidBleAddress(config.bleDeviceAddress)) {
    if (error) {
      *error = "BLE address format must be XX:XX:XX:XX:XX:XX";
    }
    return false;
  }

  return true;
}

bool loadConfig(RuntimeConfig &outConfig,
                bool *loadedFromNvs,
                String *error) {
  RuntimeConfig config = makeDefaultConfig();

  Preferences prefs;
  // Open read-write so first boot can create namespace without noisy NOT_FOUND log.
  const bool opened = prefs.begin(kPrefsNamespace, false);
  if (!opened) {
    if (loadedFromNvs) {
      *loadedFromNvs = false;
    }
    outConfig = config;
    return true;
  }

  const uint32_t storedVersion = prefs.getULong(kConfigVersionKey, 0);
  const String blob = prefs.getString(kConfigBlobKey, "");
  prefs.end();

  if (storedVersion != kConfigVersion || blob.isEmpty()) {
    if (loadedFromNvs) {
      *loadedFromNvs = false;
    }
    outConfig = config;
    return true;
  }

  DynamicJsonDocument doc(1536);
  const auto parseErr = deserializeJson(doc, blob);
  if (parseErr || !doc.is<JsonObject>()) {
    if (loadedFromNvs) {
      *loadedFromNvs = false;
    }
    if (error) {
      *error = "Stored config is corrupted";
    }
    outConfig = config;
    return true;
  }

  fromJson(doc.as<JsonObjectConst>(), config);

  String validateErr;
  if (!validateConfig(config, &validateErr)) {
    if (loadedFromNvs) {
      *loadedFromNvs = false;
    }
    if (error) {
      *error = "Stored config failed validation: " + validateErr;
    }
    outConfig = makeDefaultConfig();
    return true;
  }

  if (loadedFromNvs) {
    *loadedFromNvs = true;
  }
  outConfig = config;
  return true;
}

bool saveConfig(const RuntimeConfig &config, String *error) {
  String validateErr;
  if (!validateConfig(config, &validateErr)) {
    if (error) {
      *error = validateErr;
    }
    return false;
  }

  DynamicJsonDocument doc(1536);
  toJson(config, doc.to<JsonObject>());

  String blob;
  serializeJson(doc, blob);

  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    if (error) {
      *error = "Failed to open NVS namespace";
    }
    return false;
  }

  const bool okVersion = prefs.putULong(kConfigVersionKey, kConfigVersion) > 0;
  const bool okBlob = prefs.putString(kConfigBlobKey, blob) > 0;
  prefs.end();

  if (!okVersion || !okBlob) {
    if (error) {
      *error = "Failed to write config into NVS";
    }
    return false;
  }

  return true;
}

bool resetConfig(String *error) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    if (error) {
      *error = "Failed to open NVS namespace";
    }
    return false;
  }

  const bool cleared = prefs.clear();
  prefs.end();

  if (!cleared) {
    if (error) {
      *error = "Failed to clear NVS config";
    }
    return false;
  }

  return true;
}

const char *gatewayAuthModeName(GatewayAuthMode mode) {
  return mode == GatewayAuthMode::Password ? "Password" : "Token";
}
