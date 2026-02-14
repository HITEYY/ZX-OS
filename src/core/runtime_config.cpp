#include "runtime_config.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>

#include "board_pins.h"
#include "user_config.h"

namespace {

constexpr const char *kPrefsNamespace = "oc_cfg";
constexpr const char *kConfigVersionKey = "cfg_ver";
constexpr const char *kConfigBlobKey = "cfg_blob";
constexpr uint32_t kConfigVersion = 2;
constexpr const char *kSdConfigPath = "/oc_cfg.json";
constexpr const char *kSdConfigTempPath = "/oc_cfg.tmp";
constexpr uint32_t kSdSpiFrequencyHz = 25000000UL;

void fromJson(const JsonObjectConst &obj, RuntimeConfig &config);

bool isPlaceholder(const char *value) {
  if (!value || !value[0]) {
    return true;
  }
  return strncmp(value, "REPLACE_WITH_", 13) == 0;
}

bool startsWithWsScheme(const String &url) {
  return url.startsWith("ws://") || url.startsWith("wss://");
}

bool mountSd(String *reason = nullptr) {
  pinMode(boardpins::kTftCs, OUTPUT);
  digitalWrite(boardpins::kTftCs, HIGH);
  pinMode(boardpins::kCc1101Cs, OUTPUT);
  digitalWrite(boardpins::kCc1101Cs, HIGH);
  pinMode(boardpins::kSdCs, OUTPUT);
  digitalWrite(boardpins::kSdCs, HIGH);

  SPIClass *spiBus = &TFT_eSPI::getSPIinstance();
  const bool mounted = SD.begin(boardpins::kSdCs,
                                *spiBus,
                                kSdSpiFrequencyHz,
                                "/sd",
                                8,
                                false);
  if (!mounted && reason) {
    *reason = "SD mount failed";
  }
  return mounted;
}

bool parseConfigBlob(const String &blob,
                     RuntimeConfig &outConfig,
                     String *error = nullptr) {
  DynamicJsonDocument doc(4096);
  const auto parseErr = deserializeJson(doc, blob);
  if (parseErr || !doc.is<JsonObject>()) {
    if (error) {
      *error = "Config parse failed";
    }
    return false;
  }

  RuntimeConfig parsed = outConfig;
  fromJson(doc.as<JsonObjectConst>(), parsed);

  String validateErr;
  if (!validateConfig(parsed, &validateErr)) {
    if (error) {
      *error = "Config validation failed: " + validateErr;
    }
    return false;
  }

  outConfig = parsed;
  return true;
}

bool readConfigFromSd(RuntimeConfig &outConfig,
                      bool &found,
                      String *error = nullptr) {
  found = false;

  String mountErr;
  if (!mountSd(&mountErr)) {
    return true;
  }

  if (!SD.exists(kSdConfigPath)) {
    return true;
  }

  File file = SD.open(kSdConfigPath, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    if (error) {
      *error = "SD config open failed";
    }
    return false;
  }

  const String blob = file.readString();
  file.close();

  if (blob.isEmpty()) {
    if (error) {
      *error = "SD config is empty";
    }
    return false;
  }

  RuntimeConfig parsed = makeDefaultConfig();
  String parseErr;
  if (!parseConfigBlob(blob, parsed, &parseErr)) {
    if (error) {
      *error = "SD " + parseErr;
    }
    return false;
  }

  outConfig = parsed;
  found = true;
  return true;
}

bool readConfigFromNvs(RuntimeConfig &outConfig,
                       bool &found,
                       String *error = nullptr) {
  found = false;

  Preferences prefs;
  // Open read-write so first boot can create namespace without noisy NOT_FOUND log.
  const bool opened = prefs.begin(kPrefsNamespace, false);
  if (!opened) {
    return true;
  }

  const uint32_t storedVersion = prefs.getULong(kConfigVersionKey, 0);
  const String blob = prefs.getString(kConfigBlobKey, "");
  prefs.end();

  if (storedVersion != kConfigVersion || blob.isEmpty()) {
    return true;
  }

  RuntimeConfig parsed = makeDefaultConfig();
  String parseErr;
  if (!parseConfigBlob(blob, parsed, &parseErr)) {
    if (error) {
      *error = "NVS " + parseErr;
    }
    return false;
  }

  outConfig = parsed;
  found = true;
  return true;
}

bool writeConfigToSd(const String &blob, String *error = nullptr) {
  String mountErr;
  if (!mountSd(&mountErr)) {
    if (error) {
      *error = mountErr;
    }
    return false;
  }

  if (SD.exists(kSdConfigTempPath)) {
    SD.remove(kSdConfigTempPath);
  }

  File temp = SD.open(kSdConfigTempPath, FILE_WRITE);
  if (!temp || temp.isDirectory()) {
    if (temp) {
      temp.close();
    }
    if (error) {
      *error = "SD temp write open failed";
    }
    return false;
  }

  const size_t written = temp.print(blob);
  temp.close();
  if (written != blob.length()) {
    SD.remove(kSdConfigTempPath);
    if (error) {
      *error = "SD write failed";
    }
    return false;
  }

  if (SD.exists(kSdConfigPath)) {
    SD.remove(kSdConfigPath);
  }
  if (!SD.rename(kSdConfigTempPath, kSdConfigPath)) {
    SD.remove(kSdConfigTempPath);
    if (error) {
      *error = "SD rename failed";
    }
    return false;
  }
  return true;
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

bool isValidGithubRepoSlug(const String &repoSlug) {
  if (repoSlug.isEmpty()) {
    return true;
  }
  const int slash = repoSlug.indexOf('/');
  if (slash <= 0 || slash >= static_cast<int>(repoSlug.length()) - 1) {
    return false;
  }
  if (repoSlug.indexOf('/', static_cast<unsigned int>(slash + 1)) >= 0) {
    return false;
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
  obj["appMarketGithubRepo"] = config.appMarketGithubRepo;
  obj["appMarketReleaseAsset"] = config.appMarketReleaseAsset;
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
  config.appMarketGithubRepo =
      String(static_cast<const char *>(obj["appMarketGithubRepo"] |
                                       USER_APPMARKET_GITHUB_REPO));
  config.appMarketReleaseAsset =
      String(static_cast<const char *>(obj["appMarketReleaseAsset"] |
                                       USER_APPMARKET_RELEASE_ASSET));
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
  if (!isPlaceholder(USER_APPMARKET_GITHUB_REPO)) {
    config.appMarketGithubRepo = USER_APPMARKET_GITHUB_REPO;
  }
  if (!isPlaceholder(USER_APPMARKET_RELEASE_ASSET)) {
    config.appMarketReleaseAsset = USER_APPMARKET_RELEASE_ASSET;
  }

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

  if (!isValidGithubRepoSlug(config.appMarketGithubRepo)) {
    if (error) {
      *error = "APPMarket GitHub repo must be owner/repo";
    }
    return false;
  }

  return true;
}

bool loadConfig(RuntimeConfig &outConfig,
                ConfigLoadSource *source,
                bool *loadedFromNvs,
                String *error) {
  RuntimeConfig config = makeDefaultConfig();
  outConfig = config;
  if (source) {
    *source = ConfigLoadSource::Defaults;
  }
  if (loadedFromNvs) {
    *loadedFromNvs = false;
  }
  if (error) {
    error->clear();
  }

  RuntimeConfig sdConfig = config;
  bool sdFound = false;
  String sdErr;
  if (readConfigFromSd(sdConfig, sdFound, &sdErr) && sdFound) {
    outConfig = sdConfig;
    if (source) {
      *source = ConfigLoadSource::SdCard;
    }
    return true;
  }

  RuntimeConfig nvsConfig = config;
  bool nvsFound = false;
  String nvsErr;
  if (readConfigFromNvs(nvsConfig, nvsFound, &nvsErr) && nvsFound) {
    outConfig = nvsConfig;
    if (source) {
      *source = ConfigLoadSource::Nvs;
    }
    if (loadedFromNvs) {
      *loadedFromNvs = true;
    }
    if (error && !sdErr.isEmpty()) {
      *error = sdErr + " (using NVS backup)";
    }
    return true;
  }

  if (error) {
    if (!sdErr.isEmpty() && !nvsErr.isEmpty()) {
      *error = sdErr + "; " + nvsErr;
    } else if (!sdErr.isEmpty()) {
      *error = sdErr;
    } else if (!nvsErr.isEmpty()) {
      *error = nvsErr;
    }
  }

  return true;
}

bool saveConfig(const RuntimeConfig &config, String *error) {
  if (error) {
    error->clear();
  }

  String validateErr;
  if (!validateConfig(config, &validateErr)) {
    if (error) {
      *error = validateErr;
    }
    return false;
  }

  DynamicJsonDocument doc(4096);
  toJson(config, doc.to<JsonObject>());

  String blob;
  serializeJson(doc, blob);

  String sdErr;
  if (!writeConfigToSd(blob, &sdErr)) {
    if (error) {
      *error = "Failed to write config to SD: " + sdErr;
    }
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    Serial.println("[config] warning: failed to open NVS namespace for backup");
    return true;
  }

  const bool okVersion = prefs.putULong(kConfigVersionKey, kConfigVersion) > 0;
  const bool okBlob = prefs.putString(kConfigBlobKey, blob) > 0;
  prefs.end();

  if (!okVersion || !okBlob) {
    Serial.println("[config] warning: failed to write NVS backup");
  }

  return true;
}

bool resetConfig(String *error) {
  if (error) {
    error->clear();
  }

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

  String mountErr;
  if (!mountSd(&mountErr)) {
    // SD card missing/unavailable: treat as reset complete for NVS.
    return true;
  }

  if (SD.exists(kSdConfigPath) && !SD.remove(kSdConfigPath)) {
    if (error) {
      *error = "Failed to remove SD config file";
    }
    return false;
  }
  if (SD.exists(kSdConfigTempPath) && !SD.remove(kSdConfigTempPath)) {
    if (error) {
      *error = "Failed to remove SD temp config file";
    }
    return false;
  }

  return true;
}

const char *gatewayAuthModeName(GatewayAuthMode mode) {
  return mode == GatewayAuthMode::Password ? "Password" : "Token";
}
