#include "file_explorer_app.h"

#if __has_include(<Audio.h>)
#include <Audio.h>
#endif
#include <SD.h>
#include <SPI.h>
#include <lvgl.h>

#include <algorithm>
#include <vector>

#include "../core/board_pins.h"
#include "../core/shared_spi_bus.h"
#include "../ui/ui_runtime.h"

namespace {

struct FsEntry {
  String fullPath;
  String label;
  bool isDirectory = false;
  uint64_t size = 0;
};

bool gSdMounted = false;

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

String cardTypeName(uint8_t cardType) {
  switch (cardType) {
    case CARD_MMC:
      return "MMC";
    case CARD_SD:
      return "SDSC";
    case CARD_SDHC:
      return "SDHC/SDXC";
    case CARD_NONE:
    default:
      return "None";
  }
}

bool ensureSdMounted(bool forceMount, String *error) {
  if (gSdMounted && !forceMount) {
    return true;
  }

#if HAL_HAS_DISPLAY
  pinMode(boardpins::kTftCs, OUTPUT);
  digitalWrite(boardpins::kTftCs, HIGH);
#endif
#if HAL_HAS_CC1101
  pinMode(boardpins::kCc1101Cs, OUTPUT);
  digitalWrite(boardpins::kCc1101Cs, HIGH);
#endif
#if HAL_HAS_SD_CARD
  pinMode(boardpins::kSdCs, OUTPUT);
  digitalWrite(boardpins::kSdCs, HIGH);

  SPIClass *spiBus = sharedspi::bus();
  const bool mounted = SD.begin(boardpins::kSdCs,
                                *spiBus,
                                25000000,
                                "/sd",
                                8,
                                false);

  gSdMounted = mounted;
  if (!mounted && error) {
    *error = "SD mount failed";
  }
  return mounted;
#else
  gSdMounted = false;
  if (error) {
    *error = "SD card not available";
  }
  return false;
#endif
}

bool listDirectory(const String &path,
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

  File entry = dir.openNextFile();
  while (entry) {
    const String rawName = String(entry.name());
    if (!rawName.isEmpty()) {
      FsEntry item;
      item.fullPath = buildChildPath(path, rawName);
      item.isDirectory = entry.isDirectory();
      item.size = entry.size();

      item.label = item.isDirectory ? "[D] " : "[F] ";
      item.label += baseName(item.fullPath);
      if (!item.isDirectory) {
        item.label += " (" + formatBytes(item.size) + ")";
      }

      outEntries.push_back(item);
    }

    entry.close();
    entry = dir.openNextFile();
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

void showSdInfo(AppContext &ctx,
                const std::function<void()> &backgroundTick) {
  String err;
  if (!ensureSdMounted(false, &err)) {
    ctx.uiRuntime->showToast("SD Card",
                      err.isEmpty() ? String("Mount failed") : err,
                      1800,
                      backgroundTick);
    return;
  }

  const uint8_t type = SD.cardType();
  const uint64_t cardSize = SD.cardSize();
  const uint64_t totalBytes = SD.totalBytes();
  const uint64_t usedBytes = SD.usedBytes();
  const uint64_t freeBytes = totalBytes > usedBytes ? totalBytes - usedBytes : 0;

  std::vector<String> lines;
  lines.push_back("Card Type: " + cardTypeName(type));
  lines.push_back("Card Size: " + formatBytes(cardSize));
  lines.push_back("FS Total: " + formatBytes(totalBytes));
  lines.push_back("FS Used: " + formatBytes(usedBytes));
  lines.push_back("FS Free: " + formatBytes(freeBytes));
  lines.push_back("Mount Point: /sd");

  ctx.uiRuntime->showInfo("SD Card Info", lines, backgroundTick, "OK/BACK Exit");
}

String sanitizeTextLine(const String &input) {
  String out;
  out.reserve(input.length());

  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[static_cast<unsigned int>(i)];
    if ((c >= 32 && c <= 126) || c == '\t') {
      out += c;
    } else {
      out += '.';
    }
  }
  return out;
}

bool isImageFilePath(const String &path) {
  String lower = path;
  lower.toLowerCase();
  return lower.endsWith(".png") ||
         lower.endsWith(".jpg") ||
         lower.endsWith(".jpeg") ||
         lower.endsWith(".jeg") ||
         lower.endsWith(".bmp");
}

bool isAudioFilePath(const String &path) {
  String lower = path;
  lower.toLowerCase();
  return lower.endsWith(".wav") ||
         lower.endsWith(".mp3") ||
         lower.endsWith(".ogg") ||
         lower.endsWith(".aac") ||
         lower.endsWith(".m4a") ||
         lower.endsWith(".flac");
}

String toLvglSdPath(const String &sdPath) {
  if (sdPath.startsWith("/")) {
    return String("S:") + sdPath;
  }
  return String("S:/") + sdPath;
}

String formatDurationSeconds(uint32_t totalSec) {
  const uint32_t hours = totalSec / 3600U;
  const uint32_t mins = (totalSec % 3600U) / 60U;
  const uint32_t secs = totalSec % 60U;

  char buf[20];
  if (hours > 0U) {
    snprintf(buf,
             sizeof(buf),
             "%lu:%02lu:%02lu",
             static_cast<unsigned long>(hours),
             static_cast<unsigned long>(mins),
             static_cast<unsigned long>(secs));
  } else {
    snprintf(buf,
             sizeof(buf),
             "%lu:%02lu",
             static_cast<unsigned long>(mins),
             static_cast<unsigned long>(secs));
  }
  return String(buf);
}

void showFileInfo(AppContext &ctx,
                  const FsEntry &entry,
                  const std::function<void()> &backgroundTick) {
  std::vector<String> lines;
  lines.push_back("Path: " + entry.fullPath);
  lines.push_back("Type: " + String(entry.isDirectory ? "Directory" : "File"));
  lines.push_back("Size: " + formatBytes(entry.size));
  ctx.uiRuntime->showInfo("File Info", lines, backgroundTick, "OK/BACK Exit");
}

void previewTextFile(AppContext &ctx,
                     const FsEntry &entry,
                     const std::function<void()> &backgroundTick) {
  File file = SD.open(entry.fullPath.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    ctx.uiRuntime->showToast("Preview", "File open failed", 1500, backgroundTick);
    return;
  }

  std::vector<String> lines;
  lines.push_back(trimMiddle(entry.fullPath, 30));
  lines.push_back("Size: " + formatBytes(entry.size));
  lines.push_back("----------------");

  constexpr int kMaxPreviewLines = 20;
  int shown = 0;

  while (file.available() && shown < kMaxPreviewLines) {
    String line = file.readStringUntil('\n');
    line.replace("\r", "");
    line = sanitizeTextLine(line);
    if (line.length() > 44) {
      line = line.substring(0, 41) + "...";
    }
    if (line.isEmpty()) {
      line = " ";
    }
    lines.push_back(line);
    ++shown;
  }

  if (shown == 0) {
    lines.push_back("(empty file)");
  } else if (file.available()) {
    lines.push_back("... (truncated)");
  }

  file.close();

  ctx.uiRuntime->showInfo("File Preview", lines, backgroundTick, "OK/BACK Exit");
}

void viewImageFile(AppContext &ctx,
                   const FsEntry &entry,
                   const std::function<void()> &backgroundTick) {
  const String lvPath = toLvglSdPath(entry.fullPath);

  lv_image_header_t header;
  if (lv_image_decoder_get_info(lvPath.c_str(), &header) != LV_RESULT_OK) {
    ctx.uiRuntime->showToast("Image",
                             "Unsupported image format",
                             1700,
                             backgroundTick);
    return;
  }

  lv_obj_t *screen = lv_screen_active();
  if (!screen) {
    ctx.uiRuntime->showToast("Image", "Display not ready", 1400, backgroundTick);
    return;
  }

  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

  lv_obj_t *nameLabel = lv_label_create(screen);
  const String fileName = trimMiddle(baseName(entry.fullPath), 34);
  lv_label_set_text(nameLabel, fileName.c_str());
  lv_obj_set_style_text_color(nameLabel, lv_color_white(), 0);
  lv_obj_align(nameLabel, LV_ALIGN_TOP_MID, 0, 2);

  lv_obj_t *metaLabel = lv_label_create(screen);
  const String meta = String(static_cast<unsigned long>(header.w)) + "x" +
                      String(static_cast<unsigned long>(header.h));
  lv_label_set_text(metaLabel, meta.c_str());
  lv_obj_set_style_text_color(metaLabel, lv_color_hex(0xB0B0B0), 0);
  lv_obj_align(metaLabel, LV_ALIGN_TOP_MID, 0, 18);

  lv_obj_t *hintLabel = lv_label_create(screen);
  lv_label_set_text(hintLabel, "OK/BACK Exit");
  lv_obj_set_style_text_color(hintLabel, lv_color_hex(0x9A9A9A), 0);
  lv_obj_align(hintLabel, LV_ALIGN_BOTTOM_MID, 0, -2);

  lv_obj_t *image = lv_image_create(screen);
  lv_image_set_src(image, lvPath.c_str());
  lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CENTER);

  lv_display_t *display = lv_display_get_default();
  const int screenW = display ? lv_display_get_horizontal_resolution(display) : 320;
  const int screenH = display ? lv_display_get_vertical_resolution(display) : 170;
  const int viewportW = std::max(1, screenW - 8);
  const int viewportH = std::max(1, screenH - 52);

  uint32_t zoom = 256U;
  if (header.w > 0U && header.h > 0U) {
    const uint32_t zx = static_cast<uint32_t>(
        (static_cast<uint64_t>(viewportW) * 256ULL) / static_cast<uint64_t>(header.w));
    const uint32_t zy = static_cast<uint32_t>(
        (static_cast<uint64_t>(viewportH) * 256ULL) / static_cast<uint64_t>(header.h));
    zoom = std::min(zx, zy);
    if (zoom > 256U) {
      zoom = 256U;
    }
    if (zoom < 8U) {
      zoom = 8U;
    }
  }
  lv_image_set_scale(image, zoom);
  lv_obj_align(image, LV_ALIGN_CENTER, 0, 8);

  ctx.uiRuntime->resetInputState();
  while (true) {
    ctx.uiRuntime->tick();
    const UiEvent ev = ctx.uiRuntime->pollInput();
    if (ev.back || ev.ok || ev.okLong) {
      break;
    }
    if (backgroundTick) {
      backgroundTick();
    }
    delay(4);
  }
  ctx.uiRuntime->resetInputState();
}

void playAudioFile(AppContext &ctx,
                   const FsEntry &entry,
                   const std::function<void()> &backgroundTick) {
#if USER_AUDIO_I2S_BCLK_PIN < 0 || USER_AUDIO_I2S_LRCLK_PIN < 0 || \
    USER_AUDIO_I2S_DOUT_PIN < 0
  ctx.uiRuntime->showToast("Audio",
                           "I2S output pins are disabled",
                           1800,
                           backgroundTick);
  return;
#else
  Audio audio;
  audio.setPinout(USER_AUDIO_I2S_BCLK_PIN,
                  USER_AUDIO_I2S_LRCLK_PIN,
                  USER_AUDIO_I2S_DOUT_PIN);
  audio.setVolume(std::max(0, std::min(21, USER_AUDIO_PLAYBACK_VOLUME)));

  if (!audio.connecttoFS(SD, entry.fullPath.c_str())) {
    ctx.uiRuntime->showToast("Audio", "Playback start failed", 1700, backgroundTick);
    return;
  }

  lv_obj_t *screen = lv_screen_active();
  if (!screen) {
    audio.stopSong();
    ctx.uiRuntime->showToast("Audio", "Display not ready", 1400, backgroundTick);
    return;
  }

  lv_obj_clean(screen);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x07090C), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

  lv_obj_t *nameLabel = lv_label_create(screen);
  const String fileName = trimMiddle(baseName(entry.fullPath), 34);
  lv_label_set_text(nameLabel, fileName.c_str());
  lv_obj_set_style_text_color(nameLabel, lv_color_white(), 0);
  lv_obj_align(nameLabel, LV_ALIGN_TOP_MID, 0, 4);

  lv_obj_t *stateLabel = lv_label_create(screen);
  lv_label_set_text(stateLabel, "Playing");
  lv_obj_set_style_text_color(stateLabel, lv_color_hex(0x7BE07B), 0);
  lv_obj_align(stateLabel, LV_ALIGN_CENTER, 0, -14);

  lv_obj_t *timeLabel = lv_label_create(screen);
  lv_label_set_text(timeLabel, "0:00");
  lv_obj_set_style_text_color(timeLabel, lv_color_hex(0xD4DCE8), 0);
  lv_obj_align(timeLabel, LV_ALIGN_CENTER, 0, 10);

  lv_obj_t *hintLabel = lv_label_create(screen);
  lv_label_set_text(hintLabel, "OK Pause/Resume  BACK Exit");
  lv_obj_set_style_text_color(hintLabel, lv_color_hex(0x9AA6B8), 0);
  lv_obj_align(hintLabel, LV_ALIGN_BOTTOM_MID, 0, -4);

  bool paused = false;
  bool exitRequested = false;
  unsigned long lastUiUpdateMs = 0;
  ctx.uiRuntime->resetInputState();

  while (!exitRequested) {
    audio.loop();
    ctx.uiRuntime->tick();

    const UiEvent ev = ctx.uiRuntime->pollInput();
    if (ev.back || ev.okLong) {
      exitRequested = true;
    } else if (ev.ok) {
      paused = audio.pauseResume();
      lv_label_set_text(stateLabel, paused ? "Paused" : "Playing");
      lv_obj_set_style_text_color(stateLabel,
                                  paused ? lv_color_hex(0xF4CE6A) : lv_color_hex(0x7BE07B),
                                  0);
    }

    const unsigned long now = millis();
    if (lastUiUpdateMs == 0 || now - lastUiUpdateMs >= 200UL) {
      lastUiUpdateMs = now;
      const uint32_t currentSec = audio.getAudioCurrentTime();
      const uint32_t durationSec = audio.getAudioFileDuration();
      String line = formatDurationSeconds(currentSec);
      if (durationSec > 0U) {
        line += " / ";
        line += formatDurationSeconds(durationSec);
      }
      lv_label_set_text(timeLabel, line.c_str());
    }

    if (!audio.isRunning()) {
      break;
    }

    if (backgroundTick) {
      backgroundTick();
    }
    delay(4);
  }

  const bool endedNaturally = !exitRequested && !audio.isRunning();
  audio.stopSong();
  ctx.uiRuntime->resetInputState();

  if (endedNaturally) {
    ctx.uiRuntime->showToast("Audio", "Playback completed", 900, backgroundTick);
  }
#endif
}

void runFileMenu(AppContext &ctx,
                 const FsEntry &entry,
                 const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    const bool imageFile = isImageFilePath(entry.fullPath);
    const bool audioFile = isAudioFilePath(entry.fullPath);

    int actionInfo = -1;
    int actionViewImage = -1;
    int actionPlayAudio = -1;
    int actionPreviewText = -1;
    int actionBack = -1;

    std::vector<String> menu;
    actionInfo = static_cast<int>(menu.size());
    menu.push_back("Info");

    if (imageFile) {
      actionViewImage = static_cast<int>(menu.size());
      menu.push_back("View Image");
    }
    if (audioFile) {
      actionPlayAudio = static_cast<int>(menu.size());
      menu.push_back("Play Audio");
    }

    actionPreviewText = static_cast<int>(menu.size());
    menu.push_back("Preview Text");

    actionBack = static_cast<int>(menu.size());
    menu.push_back("Back");

    const String subtitle = trimMiddle(baseName(entry.fullPath), 24);
    const int choice = ctx.uiRuntime->menuLoop("File",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        subtitle);
    if (choice < 0 || choice == actionBack) {
      return;
    }

    selected = choice;
    if (choice == actionInfo) {
      showFileInfo(ctx, entry, backgroundTick);
    } else if (choice == actionViewImage) {
      viewImageFile(ctx, entry, backgroundTick);
    } else if (choice == actionPlayAudio) {
      playAudioFile(ctx, entry, backgroundTick);
    } else if (choice == actionPreviewText) {
      previewTextFile(ctx, entry, backgroundTick);
    }
  }
}

bool deletePathRecursive(const String &path,
                         const std::function<void()> &backgroundTick,
                         String *error) {
  File node = SD.open(path.c_str(), FILE_READ);
  if (!node) {
    if (error) {
      *error = "Open failed: " + path;
    }
    return false;
  }

  const bool isDir = node.isDirectory();
  if (!isDir) {
    node.close();
    if (!SD.remove(path.c_str())) {
      if (error) {
        *error = "Delete failed: " + path;
      }
      return false;
    }
    if (backgroundTick) {
      backgroundTick();
    }
    return true;
  }

  std::vector<String> childPaths;
  File child = node.openNextFile();
  while (child) {
    const String childName = String(child.name());
    if (!childName.isEmpty()) {
      childPaths.push_back(buildChildPath(path, childName));
    }
    child.close();
    child = node.openNextFile();
  }
  node.close();

  for (std::vector<String>::const_iterator it = childPaths.begin();
       it != childPaths.end();
       ++it) {
    if (!deletePathRecursive(*it, backgroundTick, error)) {
      return false;
    }
  }

  if (path != "/") {
    if (!SD.rmdir(path.c_str())) {
      if (error) {
        *error = "Dir remove failed: " + path;
      }
      return false;
    }
  }

  if (backgroundTick) {
    backgroundTick();
  }
  return true;
}

bool quickFormatSd(const std::function<void()> &backgroundTick,
                   String *error) {
  String mountErr;
  if (!ensureSdMounted(false, &mountErr)) {
    if (error) {
      *error = mountErr;
    }
    return false;
  }

  std::vector<FsEntry> rootEntries;
  if (!listDirectory("/", rootEntries, error)) {
    return false;
  }

  for (std::vector<FsEntry>::const_iterator it = rootEntries.begin();
       it != rootEntries.end();
       ++it) {
    if (!deletePathRecursive(it->fullPath, backgroundTick, error)) {
      return false;
    }
  }

  if (backgroundTick) {
    backgroundTick();
  }
  return true;
}

void formatSdCard(AppContext &ctx,
                  const std::function<void()> &backgroundTick) {
  String err;
  if (!ensureSdMounted(false, &err)) {
    ctx.uiRuntime->showToast("SD Card",
                      err.isEmpty() ? String("Mount failed") : err,
                      1800,
                      backgroundTick);
    return;
  }

  if (!ctx.uiRuntime->confirm("Format SD",
                       "Quick format: delete all files?",
                       backgroundTick,
                       "Format",
                       "Cancel")) {
    return;
  }

  if (!ctx.uiRuntime->confirm("Confirm Again",
                       "This cannot be undone",
                       backgroundTick,
                       "Format",
                       "Cancel")) {
    return;
  }

  if (!quickFormatSd(backgroundTick, &err)) {
    ctx.uiRuntime->showToast("SD Format",
                      err.isEmpty() ? String("Format failed") : err,
                      2000,
                      backgroundTick);
    return;
  }

  ctx.uiRuntime->showToast("SD Format", "Quick format completed", 1600, backgroundTick);
}

void browseSd(AppContext &ctx,
              const std::function<void()> &backgroundTick) {
  String err;
  if (!ensureSdMounted(false, &err)) {
    ctx.uiRuntime->showToast("SD Card",
                      err.isEmpty() ? String("Mount failed") : err,
                      1800,
                      backgroundTick);
    return;
  }

  String currentPath = "/";
  int selected = 0;

  while (true) {
    std::vector<FsEntry> entries;
    if (!listDirectory(currentPath, entries, &err)) {
      ctx.uiRuntime->showToast("Explorer",
                        err.isEmpty() ? String("Read failed") : err,
                        1700,
                        backgroundTick);
      return;
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

    const String subtitle = "Path: " + trimMiddle(currentPath, 23);
    const int choice = ctx.uiRuntime->menuLoop("File Explorer",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Open  BACK Exit",
                                        subtitle);

    if (choice < 0) {
      return;
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
      return;
    }
    if (idx < 0 || idx >= entryCount) {
      continue;
    }

    const FsEntry selectedEntry = entries[static_cast<size_t>(idx)];
    if (selectedEntry.isDirectory) {
      currentPath = selectedEntry.fullPath;
      selected = 0;
      continue;
    }

    runFileMenu(ctx, selectedEntry, backgroundTick);
  }
}

}  // namespace

void runFileExplorerApp(AppContext &ctx,
                        const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    menu.push_back("SD Card Info");
    menu.push_back("Browse SD");
    menu.push_back("Format SD Card");
    menu.push_back("Remount SD");
    menu.push_back("Back");

    const String subtitle = gSdMounted ? "SD: Mounted" : "SD: Not mounted";
    const int choice = ctx.uiRuntime->menuLoop("File Explorer",
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
      showSdInfo(ctx, backgroundTick);
    } else if (choice == 1) {
      browseSd(ctx, backgroundTick);
    } else if (choice == 2) {
      formatSdCard(ctx, backgroundTick);
    } else if (choice == 3) {
      String err;
      if (ensureSdMounted(true, &err)) {
        ctx.uiRuntime->showToast("SD Card", "Mounted", 1200, backgroundTick);
      } else {
        ctx.uiRuntime->showToast("SD Card",
                          err.isEmpty() ? String("Mount failed") : err,
                          1800,
                          backgroundTick);
      }
    }
  }
}
