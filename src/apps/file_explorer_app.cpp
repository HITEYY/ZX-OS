#include "file_explorer_app.h"

#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>

#include <algorithm>
#include <vector>

#include "../core/board_pins.h"
#include "../ui/ui_shell.h"

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

  gSdMounted = mounted;
  if (!mounted && error) {
    *error = "SD mount failed";
  }
  return mounted;
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
    ctx.ui->showToast("SD Card",
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

  ctx.ui->showInfo("SD Card Info", lines, backgroundTick, "OK/BACK Exit");
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

void showFileInfo(AppContext &ctx,
                  const FsEntry &entry,
                  const std::function<void()> &backgroundTick) {
  std::vector<String> lines;
  lines.push_back("Path: " + entry.fullPath);
  lines.push_back("Type: " + String(entry.isDirectory ? "Directory" : "File"));
  lines.push_back("Size: " + formatBytes(entry.size));
  ctx.ui->showInfo("File Info", lines, backgroundTick, "OK/BACK Exit");
}

void previewTextFile(AppContext &ctx,
                     const FsEntry &entry,
                     const std::function<void()> &backgroundTick) {
  File file = SD.open(entry.fullPath.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    ctx.ui->showToast("Preview", "File open failed", 1500, backgroundTick);
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

  ctx.ui->showInfo("File Preview", lines, backgroundTick, "OK/BACK Exit");
}

void runFileMenu(AppContext &ctx,
                 const FsEntry &entry,
                 const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    menu.push_back("Info");
    menu.push_back("Preview Text");
    menu.push_back("Back");

    const String subtitle = trimMiddle(baseName(entry.fullPath), 24);
    const int choice = ctx.ui->menuLoop("File",
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
      showFileInfo(ctx, entry, backgroundTick);
    } else if (choice == 1) {
      previewTextFile(ctx, entry, backgroundTick);
    }
  }
}

void browseSd(AppContext &ctx,
              const std::function<void()> &backgroundTick) {
  String err;
  if (!ensureSdMounted(false, &err)) {
    ctx.ui->showToast("SD Card",
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
      ctx.ui->showToast("Explorer",
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
    const int choice = ctx.ui->menuLoop("File Explorer",
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
    menu.push_back("Remount SD");
    menu.push_back("Back");

    const String subtitle = gSdMounted ? "SD: Mounted" : "SD: Not mounted";
    const int choice = ctx.ui->menuLoop("File Explorer",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        subtitle);
    if (choice < 0 || choice == 3) {
      return;
    }

    selected = choice;

    if (choice == 0) {
      showSdInfo(ctx, backgroundTick);
    } else if (choice == 1) {
      browseSd(ctx, backgroundTick);
    } else if (choice == 2) {
      String err;
      if (ensureSdMounted(true, &err)) {
        ctx.ui->showToast("SD Card", "Mounted", 1200, backgroundTick);
      } else {
        ctx.ui->showToast("SD Card",
                          err.isEmpty() ? String("Mount failed") : err,
                          1800,
                          backgroundTick);
      }
    }
  }
}
