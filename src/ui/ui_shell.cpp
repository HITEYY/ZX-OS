#include "ui_shell.h"

#include <RotaryEncoder.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <Wire.h>

#include <time.h>

#include "user_config.h"

namespace {

constexpr uint8_t PIN_ENCODER_A = 4;
constexpr uint8_t PIN_ENCODER_B = 5;
constexpr uint8_t PIN_OK = 0;
constexpr uint8_t PIN_BACK = 6;

constexpr unsigned long kDebounceMs = 35UL;
constexpr unsigned long kLongPressMs = 750UL;

constexpr int kHeaderHeight = 22;
constexpr int kFooterHeight = 14;
constexpr int kRowHeight = 16;
constexpr unsigned long kHeaderRefreshMs = 1000UL;
constexpr unsigned long kBatteryPollMs = 5000UL;
constexpr unsigned long kNtpRetryMs = 30000UL;

int wrapIndex(int value, int count) {
  if (count <= 0) {
    return 0;
  }
  while (value < 0) {
    value += count;
  }
  while (value >= count) {
    value -= count;
  }
  return value;
}

String maskIfNeeded(const String &value, bool mask) {
  if (!mask) {
    return value;
  }
  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    out += "*";
  }
  return out;
}

String formatUptimeClock(unsigned long ms) {
  const unsigned long totalSec = ms / 1000UL;
  const unsigned long hours = (totalSec / 3600UL) % 24UL;
  const unsigned long mins = (totalSec / 60UL) % 60UL;

  char buf[6];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", hours, mins);
  return String(buf);
}

}  // namespace

class UIShell::Impl {
 public:
  Impl()
      : encoder(PIN_ENCODER_A,
                PIN_ENCODER_B,
                RotaryEncoder::LatchMode::TWO03) {}

  TFT_eSPI tft;
  RotaryEncoder encoder;

  int32_t lastEncoderPos = 0;

  bool okPrev = false;
  bool backPrev = false;

  unsigned long okPressedAt = 0;
  unsigned long backPressedAt = 0;
  bool okLongFired = false;

  String statusLine;
  String headerTime;
  String headerStatus;
  int batteryPct = -1;
  bool ntpStarted = false;
  unsigned long lastNtpAttemptMs = 0;
  unsigned long lastBatteryPollMs = 0;
  unsigned long lastHeaderUpdateMs = 0;

  int readBatteryPercent() {
#if USER_BATTERY_GAUGE_ENABLED
    static bool wireReady = false;
    if (!wireReady) {
      Wire.begin(USER_BATTERY_GAUGE_SDA, USER_BATTERY_GAUGE_SCL);
      wireReady = true;
    }

    Wire.beginTransmission(USER_BATTERY_GAUGE_ADDR);
    Wire.write(USER_BATTERY_GAUGE_SOC_REG);
    if (Wire.endTransmission(false) != 0) {
      return -1;
    }

    const int readCount = Wire.requestFrom(static_cast<int>(USER_BATTERY_GAUGE_ADDR), 2);
    if (readCount < 2) {
      return -1;
    }

    const uint8_t lo = static_cast<uint8_t>(Wire.read());
    const uint8_t hi = static_cast<uint8_t>(Wire.read());
    const int pct = (static_cast<int>(hi) << 8) | static_cast<int>(lo);
    if (pct < 0 || pct > 100) {
      return -1;
    }
    return pct;
#else
    return -1;
#endif
  }

  void updateHeaderIndicators() {
    const unsigned long now = millis();
    if (now - lastHeaderUpdateMs < kHeaderRefreshMs) {
      return;
    }
    lastHeaderUpdateMs = now;

    if (WiFi.status() == WL_CONNECTED && !ntpStarted &&
        now - lastNtpAttemptMs >= kNtpRetryMs) {
      lastNtpAttemptMs = now;
      configTzTime(USER_TIMEZONE_TZ, USER_NTP_SERVER_1, USER_NTP_SERVER_2);
      ntpStarted = true;
    }

    struct tm timeInfo;
    if (getLocalTime(&timeInfo, 1)) {
      char buf[6];
      snprintf(buf, sizeof(buf), "%02d:%02d", timeInfo.tm_hour, timeInfo.tm_min);
      headerTime = String(buf);
    } else {
      headerTime = formatUptimeClock(now);
    }

    if (now - lastBatteryPollMs >= kBatteryPollMs || batteryPct < 0) {
      lastBatteryPollMs = now;
      batteryPct = readBatteryPercent();
    }

    String status = "W:";
    if (WiFi.status() == WL_CONNECTED) {
      status += String(WiFi.RSSI());
    } else {
      status += "--";
    }
    status += " B:";
    if (batteryPct >= 0) {
      status += String(batteryPct);
      status += "%";
    } else {
      status += "--";
    }
    headerStatus = status;
  }

  void drawHeader(const String &title, const String &subtitle) {
    updateHeaderIndicators();

    tft.fillScreen(TFT_BLACK);

    tft.fillRect(0, 0, tft.width(), kHeaderHeight, TFT_DARKCYAN);
    tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
    tft.setCursor(2, 2);
    tft.print(headerTime.length() > 0 ? headerTime : "--:--");

    const int statusX = tft.width() - tft.textWidth(headerStatus) - 2;
    tft.setCursor(statusX > 2 ? statusX : 2, 2);
    tft.print(headerStatus);

    tft.setCursor(4, 12);
    tft.print(title);

    if (subtitle.length() > 0) {
      tft.setTextColor(TFT_CYAN, TFT_BLACK);
      tft.setCursor(4, kHeaderHeight + 2);
      tft.print(subtitle);
    }

    if (statusLine.length() > 0) {
      tft.fillRect(0,
                   tft.height() - kFooterHeight,
                   tft.width(),
                   kFooterHeight,
                   TFT_DARKGREY);
      tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
      tft.setCursor(2, tft.height() - kFooterHeight + 2);
      tft.print(statusLine);
    }
  }

  void drawFooter(const String &footer) {
    tft.fillRect(0,
                 tft.height() - kFooterHeight,
                 tft.width(),
                 kFooterHeight,
                 TFT_NAVY);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.setCursor(2, tft.height() - kFooterHeight + 2);
    tft.print(footer);
  }

  void drawMenu(const String &title,
                const std::vector<String> &items,
                int selected,
                const String &subtitle,
                const String &footer) {
    drawHeader(title, subtitle);

    const int contentTop = subtitle.length() > 0 ? (kHeaderHeight + 16) : (kHeaderHeight + 2);
    const int contentBottom = tft.height() - kFooterHeight - 2;
    const int maxRows = (contentBottom - contentTop) / kRowHeight;

    int start = selected - (maxRows / 2);
    if (start < 0) {
      start = 0;
    }
    if (start + maxRows > static_cast<int>(items.size())) {
      start = static_cast<int>(items.size()) - maxRows;
      if (start < 0) {
        start = 0;
      }
    }

    for (int row = 0; row < maxRows; ++row) {
      const int index = start + row;
      const int y = contentTop + row * kRowHeight;

      if (index < 0 || index >= static_cast<int>(items.size())) {
        tft.fillRect(0, y, tft.width(), kRowHeight, TFT_BLACK);
        continue;
      }

      const bool isSelected = index == selected;
      const uint16_t bg = isSelected ? TFT_YELLOW : TFT_BLACK;
      const uint16_t fg = isSelected ? TFT_BLACK : TFT_WHITE;
      tft.fillRect(0, y, tft.width(), kRowHeight, bg);
      tft.setTextColor(fg, bg);
      tft.setCursor(4, y + 3);
      tft.print(items[static_cast<size_t>(index)]);
    }

    drawFooter(footer);
  }
};

UIShell::UIShell() : impl_(new Impl()) {}

void UIShell::begin() {
  pinMode(PIN_OK, INPUT_PULLUP);
  pinMode(PIN_BACK, INPUT_PULLUP);

  impl_->tft.init();
  impl_->tft.setRotation(3);
  impl_->tft.fillScreen(TFT_BLACK);
  impl_->tft.setTextFont(1);
  impl_->tft.setTextSize(1);
  impl_->tft.setTextColor(TFT_WHITE, TFT_BLACK);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  impl_->encoder.setPosition(0);
  impl_->lastEncoderPos = 0;
}

UiEvent UIShell::pollInput() {
  UiEvent event;

  impl_->encoder.tick();
  const int32_t pos = impl_->encoder.getPosition();
  const int32_t delta = pos - impl_->lastEncoderPos;
  if (delta != 0) {
    event.delta = static_cast<int>(delta);
    impl_->lastEncoderPos = pos;
  }

  const unsigned long now = millis();

  const bool okPressed = digitalRead(PIN_OK) == LOW;
  if (okPressed && !impl_->okPrev) {
    impl_->okPressedAt = now;
    impl_->okLongFired = false;
  }
  if (!okPressed && impl_->okPrev) {
    if (!impl_->okLongFired && now - impl_->okPressedAt >= kDebounceMs) {
      event.ok = true;
    }
    impl_->okPressedAt = 0;
    impl_->okLongFired = false;
  }
  if (okPressed && !impl_->okLongFired && impl_->okPressedAt > 0 &&
      now - impl_->okPressedAt >= kLongPressMs) {
    event.back = true;
    impl_->okLongFired = true;
  }
  impl_->okPrev = okPressed;

  const bool backPressed = digitalRead(PIN_BACK) == LOW;
  if (backPressed && !impl_->backPrev) {
    impl_->backPressedAt = now;
  }
  if (!backPressed && impl_->backPrev) {
    if (now - impl_->backPressedAt >= kDebounceMs) {
      event.back = true;
    }
    impl_->backPressedAt = 0;
  }
  impl_->backPrev = backPressed;

  return event;
}

void UIShell::setStatusLine(const String &line) {
  impl_->statusLine = line;
}

int UIShell::menuLoop(const String &title,
                      const std::vector<String> &items,
                      int selectedIndex,
                      const std::function<void()> &backgroundTick,
                      const String &footer,
                      const String &subtitle) {
  if (items.empty()) {
    return -1;
  }

  int selected = wrapIndex(selectedIndex, static_cast<int>(items.size()));
  bool redraw = true;
  unsigned long lastRefreshMs = millis();

  while (true) {
    const unsigned long now = millis();
    if (!redraw && now - lastRefreshMs >= kHeaderRefreshMs) {
      redraw = true;
    }

    if (redraw) {
      impl_->drawMenu(title, items, selected, subtitle, footer);
      lastRefreshMs = now;
      redraw = false;
    }

    UiEvent ev = pollInput();
    if (ev.delta != 0) {
      selected = wrapIndex(selected + (ev.delta > 0 ? 1 : -1),
                           static_cast<int>(items.size()));
      redraw = true;
    }
    if (ev.ok) {
      return selected;
    }
    if (ev.back) {
      return -1;
    }

    if (backgroundTick) {
      backgroundTick();
    }
    delay(10);
  }
}

void UIShell::showInfo(const String &title,
                       const std::vector<String> &lines,
                       const std::function<void()> &backgroundTick,
                       const String &footer) {
  const int top = kHeaderHeight + 4;
  const int bottom = impl_->tft.height() - kFooterHeight - 2;
  int maxRows = (bottom - top) / kRowHeight;
  if (maxRows < 1) {
    maxRows = 1;
  }

  int startIndex = 0;
  bool redraw = true;
  unsigned long lastRefreshMs = millis();

  while (true) {
    const int total = static_cast<int>(lines.size());
    const int maxStart = total > maxRows ? total - maxRows : 0;
    if (startIndex < 0) {
      startIndex = 0;
    }
    if (startIndex > maxStart) {
      startIndex = maxStart;
    }

    const unsigned long now = millis();
    if (!redraw && now - lastRefreshMs >= kHeaderRefreshMs) {
      redraw = true;
    }

    if (redraw) {
      impl_->drawHeader(title, "");

      for (int row = 0; row < maxRows; ++row) {
        const int y = top + row * kRowHeight;
        impl_->tft.fillRect(0, y, impl_->tft.width(), kRowHeight, TFT_BLACK);

        const int lineIndex = startIndex + row;
        if (lineIndex < 0 || lineIndex >= total) {
          continue;
        }

        impl_->tft.setTextColor(TFT_WHITE, TFT_BLACK);
        impl_->tft.setCursor(4, y);
        impl_->tft.print(lines[static_cast<size_t>(lineIndex)]);
      }

      String footerText = footer;
      if (total > maxRows) {
        footerText = "ROT Scroll  " + footer;
      }
      impl_->drawFooter(footerText);
      lastRefreshMs = now;
      redraw = false;
    }

    UiEvent ev = pollInput();
    if (ev.delta != 0 && static_cast<int>(lines.size()) > maxRows) {
      startIndex += (ev.delta > 0 ? 1 : -1);
      redraw = true;
    }
    if (ev.back || ev.ok) {
      return;
    }
    if (backgroundTick) {
      backgroundTick();
    }
    delay(10);
  }
}

bool UIShell::confirm(const String &title,
                      const String &message,
                      const std::function<void()> &backgroundTick,
                      const String &confirmLabel,
                      const String &cancelLabel) {
  impl_->drawHeader(title, message);
  std::vector<String> options;
  options.push_back(confirmLabel);
  options.push_back(cancelLabel);
  const int selected = menuLoop(title,
                                options,
                                1,
                                backgroundTick,
                                "OK Select  BACK Cancel",
                                message);
  return selected == 0;
}

bool UIShell::textInput(const String &title,
                        String &inOutValue,
                        bool mask,
                        const std::function<void()> &backgroundTick) {
  static const char *kRowsNormal[] = {
      "1234567890-=",
      "qwertyuiop[]",
      "asdfghjkl;'\\",
      "\\zxcvbnm,.?/",
  };
  static const char *kRowsShifted[] = {
      "!@#$%^&*()_+",
      "QWERTYUIOP{}",
      "ASDFGHJKL:\"|",
      "|ZXCVBNM<>//",
  };

  String working = inOutValue;
  size_t rowIndex = 0;
  int selected = 0;
  bool caps = false;

  while (true) {
    std::vector<String> entries;
    const String rowNormal = kRowsNormal[rowIndex];
    const String rowShifted = kRowsShifted[rowIndex];
    const int rowCharCount = static_cast<int>(rowNormal.length());

    for (int i = 0; i < rowCharCount; ++i) {
      const char c = caps ? rowShifted[static_cast<unsigned int>(i)]
                          : rowNormal[static_cast<unsigned int>(i)];
      entries.push_back(String(c));
    }

    entries.push_back("DONE");
    entries.push_back(caps ? "CAPS:ON" : "CAPS:OFF");
    entries.push_back("DEL");
    entries.push_back("SPACE");
    entries.push_back("CANCEL");

    selected = wrapIndex(selected, static_cast<int>(entries.size()));

    String preview = "[row ";
    preview += String(static_cast<int>(rowIndex + 1));
    preview += "/4] ";
    preview += maskIfNeeded(working, mask);

    impl_->drawMenu(title,
                    entries,
                    selected,
                    preview,
                    "BACK: Row  OK:Select");

    UiEvent ev = pollInput();
    if (ev.delta != 0) {
      selected = wrapIndex(selected + (ev.delta > 0 ? 1 : -1),
                           static_cast<int>(entries.size()));
    }

    if (ev.back) {
      rowIndex = (rowIndex + 1) % 4;
      selected = 0;
    } else if (ev.ok) {
      if (selected < rowCharCount) {
        const char c = caps ? rowShifted[static_cast<unsigned int>(selected)]
                            : rowNormal[static_cast<unsigned int>(selected)];
        working += c;
      } else {
        const int action = selected - rowCharCount;
        if (action == 0) {  // DONE
          inOutValue = working;
          return true;
        } else if (action == 1) {  // CAPS
          caps = !caps;
        } else if (action == 2) {  // DEL
          if (working.length() > 0) {
            working.remove(working.length() - 1);
          }
        } else if (action == 3) {  // SPACE
          working += " ";
        } else if (action == 4) {  // CANCEL
          return false;
        }
      }
    }

    if (backgroundTick) {
      backgroundTick();
    }
    delay(10);
  }
}

void UIShell::showToast(const String &title,
                        const String &message,
                        unsigned long showMs,
                        const std::function<void()> &backgroundTick) {
  auto drawToastFrame = [&]() {
    impl_->drawHeader(title, "");
    impl_->tft.setTextColor(TFT_WHITE, TFT_BLACK);
    impl_->tft.setCursor(4, kHeaderHeight + 8);
    impl_->tft.print(message);
    impl_->drawFooter("OK/BACK: Close");
  };

  drawToastFrame();

  const unsigned long start = millis();
  unsigned long lastRefreshMs = start;
  while (millis() - start < showMs) {
    const unsigned long now = millis();
    if (now - lastRefreshMs >= kHeaderRefreshMs) {
      drawToastFrame();
      lastRefreshMs = now;
    }

    UiEvent ev = pollInput();
    if (ev.ok || ev.back) {
      break;
    }
    if (backgroundTick) {
      backgroundTick();
    }
    delay(10);
  }
}
