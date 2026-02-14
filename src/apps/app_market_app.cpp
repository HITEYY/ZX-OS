#include "app_market_app.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <algorithm>
#include <vector>

#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "../core/board_pins.h"
#include "../core/runtime_config.h"
#include "../ui/ui_shell.h"

namespace {

constexpr const char *kAppMarketDir = "/appmarket";
constexpr const char *kLatestPackagePath = "/appmarket/latest.bin";
constexpr const char *kBackupPackagePath = "/appmarket/current_backup.bin";
constexpr size_t kTransferChunkBytes = 2048;
constexpr unsigned long kDownloadIdleTimeoutMs = 12000UL;

struct ReleaseInfo {
  String tag;
  String assetName;
  String downloadUrl;
  uint32_t size = 0;
};

struct FsEntry {
  String fullPath;
  String label;
  bool isDirectory = false;
  uint32_t size = 0;
};

bool gSdMountedForMarket = false;

String formatBytes(uint64_t bytes) {
  static const char *kUnits[] = {"B", "KB", "MB", "GB"};

  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit < 3) {
    value /= 1024.0;
    ++unit;
  }

  char buf[32];
  if (unit == 0) {
    snprintf(buf,
             sizeof(buf),
             "%llu %s",
             static_cast<unsigned long long>(bytes),
             kUnits[unit]);
  } else {
    snprintf(buf, sizeof(buf), "%.1f %s", value, kUnits[unit]);
  }
  return String(buf);
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

bool hasBinExtension(const String &pathOrNameRaw) {
  String pathOrName = pathOrNameRaw;
  pathOrName.toLowerCase();
  return pathOrName.endsWith(".bin");
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

bool ensureSdMounted(bool forceMount, String *error) {
  if (gSdMountedForMarket && !forceMount) {
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
  gSdMountedForMarket = mounted;
  if (!mounted && error) {
    *error = "SD mount failed";
  }
  return mounted;
}

bool ensureMarketDirectory(String *error) {
  File node = SD.open(kAppMarketDir, FILE_READ);
  if (node) {
    const bool isDir = node.isDirectory();
    node.close();
    if (isDir) {
      return true;
    }
    if (error) {
      *error = "Path conflict: /appmarket is file";
    }
    return false;
  }

  if (!SD.mkdir(kAppMarketDir)) {
    if (error) {
      *error = "Failed to create /appmarket";
    }
    return false;
  }
  return true;
}

bool statSdFile(const String &path, uint32_t &sizeOut) {
  sizeOut = 0;

  File file = SD.open(path.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    return false;
  }

  sizeOut = static_cast<uint32_t>(file.size());
  file.close();
  return true;
}

String normalizeRepoSlug(const String &rawInput) {
  String value = rawInput;
  value.trim();

  if (value.startsWith("https://github.com/")) {
    value.remove(0, String("https://github.com/").length());
  } else if (value.startsWith("http://github.com/")) {
    value.remove(0, String("http://github.com/").length());
  } else if (value.startsWith("github.com/")) {
    value.remove(0, String("github.com/").length());
  }

  while (value.startsWith("/")) {
    value.remove(0, 1);
  }
  while (value.endsWith("/")) {
    value.remove(value.length() - 1);
  }
  if (value.endsWith(".git")) {
    value.remove(value.length() - 4);
  }

  const int firstSlash = value.indexOf('/');
  if (firstSlash <= 0 || firstSlash >= static_cast<int>(value.length()) - 1) {
    return value;
  }

  const int secondSlash = value.indexOf('/', static_cast<unsigned int>(firstSlash + 1));
  if (secondSlash > 0) {
    value = value.substring(0, static_cast<unsigned int>(secondSlash));
  }
  return value;
}

bool resolveRepoSlug(const RuntimeConfig &config,
                     String &repoOut,
                     String *error) {
  repoOut = normalizeRepoSlug(config.appMarketGithubRepo);
  if (repoOut.isEmpty()) {
    if (error) {
      *error = "Set GitHub repo first (owner/repo)";
    }
    return false;
  }

  const int slash = repoOut.indexOf('/');
  if (slash <= 0 || slash >= static_cast<int>(repoOut.length()) - 1) {
    if (error) {
      *error = "Repo format must be owner/repo";
    }
    return false;
  }
  if (repoOut.indexOf('/', static_cast<unsigned int>(slash + 1)) >= 0) {
    if (error) {
      *error = "Repo format must be owner/repo";
    }
    return false;
  }
  return true;
}

bool httpGetSecure(const String &url,
                   String &responseOut,
                   int &httpCodeOut,
                   String *error) {
  responseOut = "";
  httpCodeOut = -1;

  if (WiFi.status() != WL_CONNECTED) {
    if (error) {
      *error = "Wi-Fi is not connected";
    }
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    if (error) {
      *error = "HTTP begin failed";
    }
    return false;
  }

  http.setTimeout(12000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "AI-cc1101-APPMarket");
  http.addHeader("Accept", "application/vnd.github+json");

  const int code = http.GET();
  httpCodeOut = code;
  if (code > 0) {
    responseOut = http.getString();
  }
  http.end();

  if (code <= 0) {
    if (error) {
      *error = "HTTP request failed";
    }
    return false;
  }
  if (code < 200 || code >= 300) {
    if (error) {
      *error = "HTTP " + String(code);
    }
    return false;
  }
  return true;
}

bool parseLatestReleaseBody(const String &body,
                            const String &preferredAssetNameRaw,
                            ReleaseInfo &infoOut,
                            String *error) {
  DynamicJsonDocument doc(32768);
  const auto parseErr = deserializeJson(doc, body);
  if (parseErr || !doc.is<JsonObject>()) {
    if (error) {
      *error = "Release JSON parse failed";
    }
    return false;
  }

  const JsonObjectConst root = doc.as<JsonObjectConst>();
  infoOut.tag = String(static_cast<const char *>(root["tag_name"] | ""));
  if (infoOut.tag.isEmpty()) {
    infoOut.tag = String(static_cast<const char *>(root["name"] | ""));
  }
  if (infoOut.tag.isEmpty()) {
    infoOut.tag = "(unknown)";
  }

  if (!root["assets"].is<JsonArrayConst>()) {
    if (error) {
      *error = "Release has no assets";
    }
    return false;
  }

  const JsonArrayConst assets = root["assets"].as<JsonArrayConst>();
  if (assets.size() == 0) {
    if (error) {
      *error = "Release has empty assets";
    }
    return false;
  }

  const String preferredAssetName = preferredAssetNameRaw;
  JsonObjectConst selected;

  if (!preferredAssetName.isEmpty()) {
    for (JsonObjectConst asset : assets) {
      const String name = String(static_cast<const char *>(asset["name"] | ""));
      if (name == preferredAssetName) {
        selected = asset;
        break;
      }
    }
  }

  if (selected.isNull()) {
    for (JsonObjectConst asset : assets) {
      const String name = String(static_cast<const char *>(asset["name"] | ""));
      if (hasBinExtension(name)) {
        selected = asset;
        break;
      }
    }
  }

  if (selected.isNull()) {
    selected = assets[0];
  }

  infoOut.assetName = String(static_cast<const char *>(selected["name"] | ""));
  infoOut.downloadUrl = String(static_cast<const char *>(selected["browser_download_url"] | ""));
  infoOut.size = selected["size"] | 0;

  if (infoOut.assetName.isEmpty() || infoOut.downloadUrl.isEmpty()) {
    if (error) {
      *error = "Release asset URL missing";
    }
    return false;
  }
  return true;
}

bool fetchLatestReleaseInfo(const RuntimeConfig &config,
                            ReleaseInfo &infoOut,
                            String *error) {
  String repo;
  if (!resolveRepoSlug(config, repo, error)) {
    return false;
  }

  const String url = "https://api.github.com/repos/" + repo + "/releases/latest";
  String body;
  int code = -1;
  String httpErr;
  if (!httpGetSecure(url, body, code, &httpErr)) {
    if (error) {
      *error = httpErr;
    }
    return false;
  }

  if (!parseLatestReleaseBody(body, config.appMarketReleaseAsset, infoOut, error)) {
    return false;
  }
  return true;
}

bool downloadUrlToSdFile(const String &url,
                         const char *destPath,
                         const std::function<void()> &backgroundTick,
                         uint32_t *bytesOut,
                         String *error) {
  if (bytesOut) {
    *bytesOut = 0;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (error) {
      *error = "Wi-Fi is not connected";
    }
    return false;
  }

  String sdErr;
  if (!ensureSdMounted(false, &sdErr)) {
    if (error) {
      *error = sdErr;
    }
    return false;
  }
  if (!ensureMarketDirectory(&sdErr)) {
    if (error) {
      *error = sdErr;
    }
    return false;
  }

  const String tempPath = String(destPath) + ".tmp";
  if (SD.exists(tempPath.c_str())) {
    SD.remove(tempPath.c_str());
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    if (error) {
      *error = "HTTP begin failed";
    }
    return false;
  }

  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "AI-cc1101-APPMarket");

  const int code = http.GET();
  if (code <= 0 || code < 200 || code >= 300) {
    const String response = code > 0 ? http.getString() : String("");
    http.end();
    if (error) {
      String msg = code <= 0 ? String("Download HTTP failed")
                             : String("HTTP ") + String(code);
      if (!response.isEmpty()) {
        msg += ": " + trimMiddle(response, 40);
      }
      *error = msg;
    }
    return false;
  }

  File file = SD.open(tempPath.c_str(), FILE_WRITE);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    http.end();
    if (error) {
      *error = "SD file open failed";
    }
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  int remain = http.getSize();
  uint8_t buffer[kTransferChunkBytes];
  uint32_t writtenTotal = 0;
  unsigned long lastProgressMs = millis();

  while (http.connected() && (remain > 0 || remain == -1)) {
    const size_t available = stream->available();
    if (available == 0) {
      if (millis() - lastProgressMs > kDownloadIdleTimeoutMs) {
        file.close();
        http.end();
        SD.remove(tempPath.c_str());
        if (error) {
          *error = "Download timeout";
        }
        return false;
      }
      delay(5);
      if (backgroundTick) {
        backgroundTick();
      }
      continue;
    }

    const size_t toRead = std::min(available, sizeof(buffer));
    const int readLen = stream->readBytes(buffer, toRead);
    if (readLen <= 0) {
      continue;
    }

    const size_t written = file.write(buffer, static_cast<size_t>(readLen));
    if (written != static_cast<size_t>(readLen)) {
      file.close();
      http.end();
      SD.remove(tempPath.c_str());
      if (error) {
        *error = "SD write failed";
      }
      return false;
    }

    writtenTotal += static_cast<uint32_t>(written);
    if (remain > 0) {
      remain -= static_cast<int>(written);
    }

    lastProgressMs = millis();
    if (backgroundTick) {
      backgroundTick();
    }
  }

  file.close();
  http.end();

  if (writtenTotal == 0) {
    SD.remove(tempPath.c_str());
    if (error) {
      *error = "Downloaded file is empty";
    }
    return false;
  }

  if (SD.exists(destPath)) {
    SD.remove(destPath);
  }
  if (!SD.rename(tempPath.c_str(), destPath)) {
    SD.remove(tempPath.c_str());
    if (error) {
      *error = "SD rename failed";
    }
    return false;
  }

  if (bytesOut) {
    *bytesOut = writtenTotal;
  }
  return true;
}

bool installFirmwareFromSd(const String &path,
                           const std::function<void()> &backgroundTick,
                           String *error) {
  String sdErr;
  if (!ensureSdMounted(false, &sdErr)) {
    if (error) {
      *error = sdErr;
    }
    return false;
  }

  File file = SD.open(path.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    if (error) {
      *error = "Firmware file open failed";
    }
    return false;
  }

  const size_t size = file.size();
  if (size == 0) {
    file.close();
    if (error) {
      *error = "Firmware file is empty";
    }
    return false;
  }

  if (!Update.begin(size, U_FLASH)) {
    file.close();
    if (error) {
      *error = String("Update begin failed: ") + Update.errorString();
    }
    return false;
  }

  uint8_t buffer[kTransferChunkBytes];
  while (file.available()) {
    const int readLen = file.read(buffer, sizeof(buffer));
    if (readLen <= 0) {
      continue;
    }

    const size_t written = Update.write(buffer, static_cast<size_t>(readLen));
    if (written != static_cast<size_t>(readLen)) {
      file.close();
      Update.abort();
      if (error) {
        *error = String("Update write failed: ") + Update.errorString();
      }
      return false;
    }

    if (backgroundTick) {
      backgroundTick();
    }
  }

  file.close();

  if (!Update.end(true)) {
    if (error) {
      *error = String("Update end failed: ") + Update.errorString();
    }
    return false;
  }
  if (!Update.isFinished()) {
    if (error) {
      *error = "Update not finished";
    }
    return false;
  }

  return true;
}

bool backupRunningFirmwareToSd(const char *destPath,
                               const std::function<void()> &backgroundTick,
                               String *error) {
  String sdErr;
  if (!ensureSdMounted(false, &sdErr)) {
    if (error) {
      *error = sdErr;
    }
    return false;
  }
  if (!ensureMarketDirectory(&sdErr)) {
    if (error) {
      *error = sdErr;
    }
    return false;
  }

  const esp_partition_t *running = esp_ota_get_running_partition();
  if (!running) {
    if (error) {
      *error = "Running partition not found";
    }
    return false;
  }

  const String tempPath = String(destPath) + ".tmp";
  if (SD.exists(tempPath.c_str())) {
    SD.remove(tempPath.c_str());
  }

  File out = SD.open(tempPath.c_str(), FILE_WRITE);
  if (!out || out.isDirectory()) {
    if (out) {
      out.close();
    }
    if (error) {
      *error = "Backup file open failed";
    }
    return false;
  }

  uint8_t buffer[kTransferChunkBytes];
  uint32_t offset = 0;
  while (offset < running->size) {
    const size_t chunk = std::min(static_cast<uint32_t>(sizeof(buffer)),
                                  static_cast<uint32_t>(running->size - offset));
    const esp_err_t rc = esp_partition_read(running,
                                            offset,
                                            buffer,
                                            chunk);
    if (rc != ESP_OK) {
      out.close();
      SD.remove(tempPath.c_str());
      if (error) {
        *error = "Partition read failed";
      }
      return false;
    }

    const size_t written = out.write(buffer, chunk);
    if (written != chunk) {
      out.close();
      SD.remove(tempPath.c_str());
      if (error) {
        *error = "Backup SD write failed";
      }
      return false;
    }

    offset += static_cast<uint32_t>(chunk);
    if (backgroundTick) {
      backgroundTick();
    }
  }

  out.close();

  if (SD.exists(destPath)) {
    SD.remove(destPath);
  }
  if (!SD.rename(tempPath.c_str(), destPath)) {
    SD.remove(tempPath.c_str());
    if (error) {
      *error = "Backup rename failed";
    }
    return false;
  }
  return true;
}

bool removeSdFileIfExists(const char *path, String *error) {
  if (!SD.exists(path)) {
    return true;
  }
  if (!SD.remove(path)) {
    if (error) {
      *error = "Delete failed";
    }
    return false;
  }
  return true;
}

bool listBinDirectory(const String &path,
                      std::vector<FsEntry> &outEntries,
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

  File node = dir.openNextFile();
  while (node) {
    const String rawName = String(node.name());
    if (!rawName.isEmpty()) {
      FsEntry entry;
      entry.fullPath = buildChildPath(path, rawName);
      entry.isDirectory = node.isDirectory();
      entry.size = static_cast<uint32_t>(node.size());

      const String name = baseName(entry.fullPath);
      if (entry.isDirectory || hasBinExtension(name)) {
        entry.label = entry.isDirectory ? "[D] " : "[BIN] ";
        entry.label += name;
        if (!entry.isDirectory) {
          entry.label += " (" + formatBytes(entry.size) + ")";
        }
        outEntries.push_back(entry);
      }
    }

    node.close();
    node = dir.openNextFile();
  }
  dir.close();

  std::sort(outEntries.begin(),
            outEntries.end(),
            [](const FsEntry &a, const FsEntry &b) {
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

bool selectBinFileFromSd(AppContext &ctx,
                         String &selectedPathOut,
                         const std::function<void()> &backgroundTick) {
  String err;
  if (!ensureSdMounted(false, &err)) {
    ctx.ui->showToast("SD Card",
                      err.isEmpty() ? String("Mount failed") : err,
                      1700,
                      backgroundTick);
    return false;
  }

  String currentPath = "/";
  int selected = 0;

  while (true) {
    std::vector<FsEntry> entries;
    if (!listBinDirectory(currentPath, entries, &err)) {
      ctx.ui->showToast("Select BIN",
                        err.isEmpty() ? String("Read failed") : err,
                        1700,
                        backgroundTick);
      return false;
    }

    std::vector<String> menu;
    if (currentPath != "/") {
      menu.push_back(".. (Up)");
    }
    for (std::vector<FsEntry>::const_iterator it = entries.begin();
         it != entries.end();
         ++it) {
      menu.push_back(it->label);
    }
    menu.push_back("Refresh");
    menu.push_back("Back");

    const int choice = ctx.ui->menuLoop("Install from SD",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        "Path: " + trimMiddle(currentPath, 22));
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

    const int count = static_cast<int>(entries.size());
    if (idx == count) {
      continue;
    }
    if (idx == count + 1) {
      return false;
    }
    if (idx < 0 || idx >= count) {
      continue;
    }

    const FsEntry picked = entries[static_cast<size_t>(idx)];
    if (picked.isDirectory) {
      currentPath = picked.fullPath;
      selected = 0;
      continue;
    }

    selectedPathOut = picked.fullPath;
    return true;
  }
}

void markDirty(AppContext &ctx) {
  ctx.configDirty = true;
}

void saveAppMarketConfig(AppContext &ctx,
                         const std::function<void()> &backgroundTick) {
  String validateErr;
  if (!validateConfig(ctx.config, &validateErr)) {
    ctx.ui->showToast("Validation", validateErr, 1800, backgroundTick);
    return;
  }

  String saveErr;
  if (!saveConfig(ctx.config, &saveErr)) {
    ctx.ui->showToast("Save Error",
                      saveErr.isEmpty() ? String("Failed to save config") : saveErr,
                      1900,
                      backgroundTick);
    return;
  }

  ctx.configDirty = false;
  ctx.ui->showToast("APPMarket", "Config saved", 1200, backgroundTick);
}

void showStatus(AppContext &ctx,
                const String &lastAction,
                const String &lastTag,
                const String &lastAsset,
                const std::function<void()> &backgroundTick) {
  String repo = normalizeRepoSlug(ctx.config.appMarketGithubRepo);

  uint32_t latestSize = 0;
  uint32_t backupSize = 0;

  String sdErr;
  if (ensureSdMounted(false, &sdErr)) {
    statSdFile(kLatestPackagePath, latestSize);
    statSdFile(kBackupPackagePath, backupSize);
  }

  std::vector<String> lines;
  lines.push_back("Wi-Fi: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected"));
  lines.push_back("Repo: " + (repo.isEmpty() ? String("(empty)") : repo));
  lines.push_back("Asset: " +
                  (ctx.config.appMarketReleaseAsset.isEmpty()
                       ? String("(auto .bin)")
                       : ctx.config.appMarketReleaseAsset));
  lines.push_back("Latest tag: " + (lastTag.isEmpty() ? String("-") : lastTag));
  lines.push_back("Latest asset: " + (lastAsset.isEmpty() ? String("-") : lastAsset));
  lines.push_back("Latest pkg: " +
                  (latestSize == 0 ? String("(none)")
                                   : String(kLatestPackagePath) + " " + formatBytes(latestSize)));
  lines.push_back("Backup pkg: " +
                  (backupSize == 0 ? String("(none)")
                                   : String(kBackupPackagePath) + " " + formatBytes(backupSize)));
  lines.push_back("Dirty config: " + String(ctx.configDirty ? "Yes" : "No"));
  if (!lastAction.isEmpty()) {
    lines.push_back("Last: " + lastAction);
  }

  ctx.ui->showInfo("APPMarket Status", lines, backgroundTick, "OK/BACK Exit");
}

bool confirmInstall(AppContext &ctx,
                    const String &title,
                    const String &message,
                    const std::function<void()> &backgroundTick) {
  if (!ctx.ui->confirm(title, message, backgroundTick, "Install", "Cancel")) {
    return false;
  }
  if (!ctx.ui->confirm("Confirm Again",
                       "Device will reboot after install",
                       backgroundTick,
                       "Install",
                       "Cancel")) {
    return false;
  }
  return true;
}

}  // namespace

void runAppMarketApp(AppContext &ctx,
                     const std::function<void()> &backgroundTick) {
  int selected = 0;
  String lastAction;
  String lastTag;
  String lastAsset;

  while (true) {
    uint32_t latestSize = 0;
    const bool latestExists = ensureSdMounted(false, nullptr) &&
                              statSdFile(kLatestPackagePath, latestSize);

    std::vector<String> menu;
    menu.push_back("Status");
    menu.push_back("GitHub Repo");
    menu.push_back("Release Asset");
    menu.push_back("Check Latest");
    menu.push_back("Download Latest to SD");
    menu.push_back(String("Install Latest ") +
                   (latestExists ? "(" + formatBytes(latestSize) + ")" : "(missing)"));
    menu.push_back("Backup Running App to SD");
    menu.push_back("Reinstall from Backup");
    menu.push_back("Install from SD .bin");
    menu.push_back("Delete Latest Package");
    menu.push_back("Delete Backup Package");
    menu.push_back("Save Config");
    menu.push_back("Back");

    String subtitle = normalizeRepoSlug(ctx.config.appMarketGithubRepo);
    if (subtitle.isEmpty()) {
      subtitle = "Set repo: owner/repo";
    } else {
      subtitle = trimMiddle(subtitle, 22);
    }
    if (ctx.configDirty) {
      subtitle += " *DIRTY";
    }

    const int choice = ctx.ui->menuLoop("APPMarket",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        subtitle);
    if (choice < 0 || choice == 12) {
      return;
    }
    selected = choice;

    if (choice == 0) {
      showStatus(ctx, lastAction, lastTag, lastAsset, backgroundTick);
      continue;
    }

    if (choice == 1) {
      String value = ctx.config.appMarketGithubRepo;
      if (!ctx.ui->textInput("GitHub Repo (owner/repo)", value, false, backgroundTick)) {
        continue;
      }
      value = normalizeRepoSlug(value);
      ctx.config.appMarketGithubRepo = value;
      markDirty(ctx);
      lastAction = "Repo updated";
      ctx.ui->showToast("APPMarket", "Repo updated", 1200, backgroundTick);
      continue;
    }

    if (choice == 2) {
      String value = ctx.config.appMarketReleaseAsset;
      if (!ctx.ui->textInput("Release Asset (.bin)", value, false, backgroundTick)) {
        continue;
      }
      value.trim();
      ctx.config.appMarketReleaseAsset = value;
      markDirty(ctx);
      lastAction = "Asset preference updated";
      ctx.ui->showToast("APPMarket", "Asset updated", 1200, backgroundTick);
      continue;
    }

    if (choice == 3) {
      ReleaseInfo info;
      String err;
      if (!fetchLatestReleaseInfo(ctx.config, info, &err)) {
        lastAction = "Latest check failed: " + err;
        ctx.ui->showToast("APPMarket", err, 1800, backgroundTick);
        continue;
      }

      lastTag = info.tag;
      lastAsset = info.assetName;
      lastAction = "Latest: " + info.tag + " / " + info.assetName;

      std::vector<String> lines;
      lines.push_back("Tag: " + info.tag);
      lines.push_back("Asset: " + info.assetName);
      lines.push_back("Size: " + formatBytes(info.size));
      lines.push_back("URL:");
      lines.push_back(trimMiddle(info.downloadUrl, 38));
      ctx.ui->showInfo("Latest Release", lines, backgroundTick, "OK/BACK Exit");
      continue;
    }

    if (choice == 4) {
      ReleaseInfo info;
      String err;
      if (!fetchLatestReleaseInfo(ctx.config, info, &err)) {
        lastAction = "Download check failed: " + err;
        ctx.ui->showToast("APPMarket", err, 1800, backgroundTick);
        continue;
      }

      uint32_t downloaded = 0;
      if (!downloadUrlToSdFile(info.downloadUrl,
                               kLatestPackagePath,
                               backgroundTick,
                               &downloaded,
                               &err)) {
        lastAction = "Download failed: " + err;
        ctx.ui->showToast("APPMarket", err, 1800, backgroundTick);
        continue;
      }

      lastTag = info.tag;
      lastAsset = info.assetName;
      lastAction = "Downloaded " + info.assetName + " (" + formatBytes(downloaded) + ")";
      ctx.ui->showToast("APPMarket", "Downloaded to SD", 1500, backgroundTick);
      continue;
    }

    if (choice == 5) {
      if (!latestExists) {
        ctx.ui->showToast("APPMarket", "Latest package not found", 1700, backgroundTick);
        continue;
      }
      if (!confirmInstall(ctx,
                          "Install Latest",
                          "Flash /appmarket/latest.bin?",
                          backgroundTick)) {
        continue;
      }

      String err;
      if (!installFirmwareFromSd(kLatestPackagePath, backgroundTick, &err)) {
        lastAction = "Install latest failed: " + err;
        ctx.ui->showToast("APPMarket", err, 1900, backgroundTick);
        continue;
      }

      ctx.ui->showToast("APPMarket", "Install complete, rebooting", 1200, backgroundTick);
      delay(300);
      ESP.restart();
      return;
    }

    if (choice == 6) {
      String err;
      if (!backupRunningFirmwareToSd(kBackupPackagePath, backgroundTick, &err)) {
        lastAction = "Backup failed: " + err;
        ctx.ui->showToast("APPMarket", err, 1900, backgroundTick);
        continue;
      }

      uint32_t backupSize = 0;
      statSdFile(kBackupPackagePath, backupSize);
      lastAction = "Backup created (" + formatBytes(backupSize) + ")";
      ctx.ui->showToast("APPMarket", "Backup saved to SD", 1500, backgroundTick);
      continue;
    }

    if (choice == 7) {
      uint32_t backupSize = 0;
      if (!statSdFile(kBackupPackagePath, backupSize)) {
        ctx.ui->showToast("APPMarket", "Backup package not found", 1700, backgroundTick);
        continue;
      }
      if (!confirmInstall(ctx,
                          "Reinstall Backup",
                          "Flash /appmarket/current_backup.bin?",
                          backgroundTick)) {
        continue;
      }

      String err;
      if (!installFirmwareFromSd(kBackupPackagePath, backgroundTick, &err)) {
        lastAction = "Reinstall failed: " + err;
        ctx.ui->showToast("APPMarket", err, 1900, backgroundTick);
        continue;
      }

      ctx.ui->showToast("APPMarket", "Reinstall complete, rebooting", 1200, backgroundTick);
      delay(300);
      ESP.restart();
      return;
    }

    if (choice == 8) {
      String path;
      if (!selectBinFileFromSd(ctx, path, backgroundTick)) {
        continue;
      }

      if (!confirmInstall(ctx,
                          "Install from SD",
                          "Flash " + trimMiddle(path, 26) + "?",
                          backgroundTick)) {
        continue;
      }

      String err;
      if (!installFirmwareFromSd(path, backgroundTick, &err)) {
        lastAction = "Install SD failed: " + err;
        ctx.ui->showToast("APPMarket", err, 1900, backgroundTick);
        continue;
      }

      ctx.ui->showToast("APPMarket", "Install complete, rebooting", 1200, backgroundTick);
      delay(300);
      ESP.restart();
      return;
    }

    if (choice == 9) {
      String err;
      if (!ensureSdMounted(false, &err)) {
        ctx.ui->showToast("APPMarket", err, 1700, backgroundTick);
        continue;
      }
      if (!removeSdFileIfExists(kLatestPackagePath, &err)) {
        lastAction = "Delete latest failed: " + err;
        ctx.ui->showToast("APPMarket", err, 1700, backgroundTick);
        continue;
      }
      lastAction = "Deleted latest package";
      ctx.ui->showToast("APPMarket", "Latest package deleted", 1300, backgroundTick);
      continue;
    }

    if (choice == 10) {
      String err;
      if (!ensureSdMounted(false, &err)) {
        ctx.ui->showToast("APPMarket", err, 1700, backgroundTick);
        continue;
      }
      if (!removeSdFileIfExists(kBackupPackagePath, &err)) {
        lastAction = "Delete backup failed: " + err;
        ctx.ui->showToast("APPMarket", err, 1700, backgroundTick);
        continue;
      }
      lastAction = "Deleted backup package";
      ctx.ui->showToast("APPMarket", "Backup package deleted", 1300, backgroundTick);
      continue;
    }

    if (choice == 11) {
      saveAppMarketConfig(ctx, backgroundTick);
      if (!ctx.configDirty) {
        lastAction = "Config saved";
      }
      continue;
    }
  }
}

