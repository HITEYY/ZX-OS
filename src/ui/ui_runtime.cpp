#include "ui_runtime.h"

#include <WiFi.h>
#include <Wire.h>
#include <SD.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <lvgl.h>

#include <time.h>
#include <string.h>

#include "../core/board_pins.h"
#include "input_adapter.h"
#include "launcher_icons.h"
#include "lvgl_port.h"
#include "user_config.h"

namespace {

constexpr int kHeaderHeight = 24;
constexpr int kSubtitleHeight = 17;
constexpr int kFooterHeight = 18;
constexpr int kRowHeight = 20;
constexpr int kSidePadding = 8;
constexpr int kMinContentHeight = 24;
constexpr lv_style_selector_t kStyleAny =
    static_cast<lv_style_selector_t>(LV_PART_MAIN) |
    static_cast<lv_style_selector_t>(LV_STATE_ANY);

constexpr uint32_t kClrBg = 0x0B0F14;
constexpr uint32_t kClrPanel = 0x121923;
constexpr uint32_t kClrPanelSoft = 0x0F151E;
constexpr uint32_t kClrBorder = 0x2A3544;
constexpr uint32_t kClrAccent = 0x58A6FF;
constexpr uint32_t kClrAccentSoft = 0x1D304B;
constexpr uint32_t kClrTextPrimary = 0xF5F7FA;
constexpr uint32_t kClrTextMuted = 0xAAB7C8;
constexpr lv_opa_t kOpa75 = static_cast<lv_opa_t>(191);
constexpr lv_opa_t kOpa85 = static_cast<lv_opa_t>(217);
constexpr lv_opa_t kOpa90 = static_cast<lv_opa_t>(230);
constexpr lv_opa_t kOpa92 = static_cast<lv_opa_t>(235);

constexpr unsigned long kHeaderRefreshMs = 1000UL;
constexpr unsigned long kBatteryPollMs = 5000UL;
constexpr unsigned long kNtpRetryMs = 30000UL;
constexpr unsigned long kSdPollMs = 8000UL;

constexpr uint32_t kLauncherBg = kClrBg;
constexpr uint32_t kLauncherPrimary = 0xEAF6FF;
constexpr uint32_t kLauncherSide = 0x2D6F93;
constexpr uint32_t kLauncherMuted = 0x8FB6CC;
constexpr uint32_t kLauncherLine = 0x1A3344;

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

String ellipsize(const String &text, size_t maxLen) {
  if (maxLen < 4 || text.length() <= maxLen) {
    return text;
  }
  const size_t keep = maxLen - 3;
  return text.substring(0, keep) + "...";
}

LauncherIconId iconIdFromLauncherIndex(int index) {
  switch (wrapIndex(index, 4)) {
    case 0:
      return LauncherIconId::AppMarket;
    case 1:
      return LauncherIconId::Settings;
    case 2:
      return LauncherIconId::FileExplorer;
    case 3:
    default:
      return LauncherIconId::OpenClaw;
  }
}

}  // namespace

class UiRuntime::Impl {
 public:
  LvglPort port;
  InputAdapter input;

  String statusLine;
  UiLanguage language = UiLanguage::English;
  String timezoneTz = USER_TIMEZONE_TZ;

  String headerTime;
  String headerStatus;
  int batteryPct = -1;
  int sdPct = -1;
  bool ntpStarted = false;
  bool launcherIconsAvailable = false;
  unsigned long lastNtpAttemptMs = 0;
  unsigned long lastBatteryPollMs = 0;
  unsigned long lastSdPollMs = 0;
  unsigned long lastHeaderUpdateMs = 0;

  lv_obj_t *progressOverlay = nullptr;
  lv_obj_t *progressPanel = nullptr;
  lv_obj_t *progressTitle = nullptr;
  lv_obj_t *progressMessage = nullptr;
  lv_obj_t *progressSpinner = nullptr;
  lv_obj_t *progressBar = nullptr;
  lv_obj_t *progressPercent = nullptr;

  bool begin() {
    if (!port.begin()) {
      return false;
    }

    input.begin(port.display());
    applyTheme();
    launcherIconsAvailable = initLauncherIcons();
    return true;
  }

  void applyTheme() {
    lv_theme_t *theme = lv_theme_default_init(port.display(),
                                              lv_palette_main(LV_PALETTE_BLUE),
                                              lv_palette_main(LV_PALETTE_BLUE_GREY),
                                              true,
                                              &lv_font_montserrat_14);
    lv_display_set_theme(port.display(), theme);
  }

  const lv_font_t *font() const {
    return &lv_font_montserrat_14;
  }

  void service(const std::function<void()> *backgroundTick = nullptr) {
    if (backgroundTick && *backgroundTick) {
      (*backgroundTick)();
    }

    input.tick();
    port.pump();
  }

  UiEvent pollInput() {
    const InputEvent ev = input.pollEvent();
    UiEvent out;
    out.delta = ev.delta;
    out.ok = ev.ok;
    out.back = ev.back;
    return out;
  }

  int readBatteryPercent() {
#if USER_BATTERY_GAUGE_ENABLED
    static bool wireReady = false;
    if (!wireReady) {
      Wire.begin(USER_BATTERY_GAUGE_SDA, USER_BATTERY_GAUGE_SCL);
      Wire.setTimeOut(5);
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
      const char *tz = timezoneTz.isEmpty() ? USER_TIMEZONE_TZ : timezoneTz.c_str();
      configTzTime(tz, USER_NTP_SERVER_1, USER_NTP_SERVER_2);
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

  void setTimezone(const String &tz) {
    String next = tz;
    next.trim();
    if (next.isEmpty()) {
      next = USER_TIMEZONE_TZ;
    }
    timezoneTz = next;
    ntpStarted = false;
    lastNtpAttemptMs = 0;
  }

  String timezone() const {
    if (timezoneTz.isEmpty()) {
      return String(USER_TIMEZONE_TZ);
    }
    return timezoneTz;
  }

  bool syncTimezoneFromIp(String *resolvedTz, String *error) {
    if (error) {
      error->clear();
    }

    if (WiFi.status() != WL_CONNECTED) {
      if (error) {
        *error = "Wi-Fi not connected";
      }
      return false;
    }

    HTTPClient http;
    http.setConnectTimeout(3500);
    http.setTimeout(4500);

    const char *url = "http://ip-api.com/json/?fields=status,timezone,message";
    if (!http.begin(url)) {
      if (error) {
        *error = "HTTP begin failed";
      }
      return false;
    }

    const int statusCode = http.GET();
    if (statusCode != HTTP_CODE_OK) {
      if (error) {
        *error = "IP lookup failed (" + String(statusCode) + ")";
      }
      http.end();
      return false;
    }

    const String payload = http.getString();
    http.end();

    DynamicJsonDocument doc(768);
    const auto jsonErr = deserializeJson(doc, payload);
    if (jsonErr) {
      if (error) {
        *error = "IP lookup parse failed";
      }
      return false;
    }

    const char *status = doc["status"] | "";
    if (strlen(status) > 0 && strcmp(status, "success") != 0) {
      if (error) {
        const char *msg = doc["message"] | "IP lookup rejected";
        *error = String(msg);
      }
      return false;
    }

    const char *zone = doc["timezone"] | "";
    String tz = String(zone);
    tz.trim();
    if (tz.isEmpty()) {
      if (error) {
        *error = "Timezone not found from IP";
      }
      return false;
    }

    setTimezone(tz);
    configTzTime(timezoneTz.c_str(), USER_NTP_SERVER_1, USER_NTP_SERVER_2);
    ntpStarted = true;
    lastNtpAttemptMs = millis();

    if (resolvedTz) {
      *resolvedTz = timezoneTz;
    }
    return true;
  }

  void updateSdPercent() {
    const unsigned long now = millis();
    if (lastSdPollMs != 0 && now - lastSdPollMs < kSdPollMs) {
      return;
    }
    lastSdPollMs = now;

    const uint64_t totalBytes = SD.totalBytes();
    if (totalBytes == 0) {
      sdPct = -1;
      return;
    }

    uint64_t usedBytes = SD.usedBytes();
    if (usedBytes > totalBytes) {
      usedBytes = totalBytes;
    }

    const uint64_t pct = (usedBytes * 100ULL + (totalBytes / 2ULL)) / totalBytes;
    sdPct = static_cast<int>(pct);
    if (sdPct < 0) {
      sdPct = 0;
    }
    if (sdPct > 100) {
      sdPct = 100;
    }
  }

  void drawBatteryIcon(lv_obj_t *parent, int x, int y) const {
    constexpr int bodyW = 18;
    constexpr int bodyH = 9;
    constexpr int capW = 2;
    constexpr int capH = 4;
    constexpr int segCount = 4;

    lv_obj_t *body = lv_obj_create(parent);
    disableScroll(body);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, x, y);
    lv_obj_set_size(body, bodyW, bodyH);
    lv_obj_set_style_radius(body, 1, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(kLauncherPrimary), 0);

    lv_obj_t *cap = lv_obj_create(parent);
    disableScroll(cap);
    lv_obj_remove_style_all(cap);
    lv_obj_set_pos(cap, x + bodyW, y + ((bodyH - capH) / 2));
    lv_obj_set_size(cap, capW, capH);
    lv_obj_set_style_radius(cap, 1, 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(cap, lv_color_hex(kLauncherPrimary), 0);

    int filled = 0;
    if (batteryPct >= 0) {
      filled = (batteryPct + 24) / 25;
      if (filled < 0) {
        filled = 0;
      }
      if (filled > segCount) {
        filled = segCount;
      }
    }

    const int usableW = bodyW - 4;
    const int segGap = 1;
    const int segW = (usableW - ((segCount - 1) * segGap)) / segCount;
    const int segH = bodyH - 4;
    for (int i = 0; i < segCount; ++i) {
      lv_obj_t *seg = lv_obj_create(body);
      disableScroll(seg);
      lv_obj_remove_style_all(seg);
      lv_obj_set_pos(seg, 2 + i * (segW + segGap), 2);
      lv_obj_set_size(seg, segW, segH);
      lv_obj_set_style_radius(seg, 1, 0);
      lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
      if (i < filled) {
        lv_obj_set_style_bg_color(seg, lv_color_hex(kLauncherPrimary), 0);
      } else {
        lv_obj_set_style_bg_color(seg, lv_color_hex(kLauncherLine), 0);
      }
    }
  }

  void setLabelFont(lv_obj_t *obj) const {
    lv_obj_set_style_text_font(obj, font(), kStyleAny);
    lv_obj_set_style_text_opa(obj, LV_OPA_COVER, kStyleAny);
    lv_obj_set_style_text_color(obj, lv_color_white(), kStyleAny);
  }

  void disableScroll(lv_obj_t *obj) const {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(obj, LV_DIR_NONE);
  }

  void prepareLabel(lv_obj_t *label) const {
    lv_obj_remove_style_all(label);
    disableScroll(label);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
  }

  void setSingleLineLabel(lv_obj_t *label,
                          int width,
                          lv_text_align_t align = LV_TEXT_ALIGN_LEFT) const {
    prepareLabel(label);
    setLabelFont(label);
    if (width < 1) {
      width = 1;
    }
    lv_obj_set_width(label, width);
    lv_obj_set_height(label, static_cast<int32_t>(font()->line_height + 2));
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(label, align, kStyleAny);
    lv_obj_set_style_pad_all(label, 0, kStyleAny);
  }

  void setWrapLabel(lv_obj_t *label, int width, int height = -1) const {
    prepareLabel(label);
    setLabelFont(label);
    if (width < 1) {
      width = 1;
    }
    lv_obj_set_width(label, width);
    if (height > 0) {
      lv_obj_set_height(label, height);
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, kStyleAny);
    lv_obj_set_style_pad_all(label, 0, kStyleAny);
  }

  void clearProgressHandles() {
    progressOverlay = nullptr;
    progressPanel = nullptr;
    progressTitle = nullptr;
    progressMessage = nullptr;
    progressSpinner = nullptr;
    progressBar = nullptr;
    progressPercent = nullptr;
  }

  void renderBase(const String &title,
                  const String &subtitle,
                  const String &footer,
                  int &contentTop,
                  int &contentBottom) {
    updateHeaderIndicators();

    lv_obj_t *screen = lv_screen_active();
    clearProgressHandles();
    lv_obj_clean(screen);
    disableScroll(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(kClrBg), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(kClrTextPrimary), 0);
    lv_obj_set_style_text_opa(screen, LV_OPA_COVER, 0);
    setLabelFont(screen);

    const int w = lv_display_get_horizontal_resolution(port.display());
    const int h = lv_display_get_vertical_resolution(port.display());
    const int frameX = 4;
    const int frameW = w - (frameX * 2);
    const int innerW = frameW - (kSidePadding * 2);

    lv_obj_t *header = lv_obj_create(screen);
    disableScroll(header);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, frameX, 4);
    lv_obj_set_size(header, frameW, kHeaderHeight);
    lv_obj_set_style_radius(header, 8, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(kClrPanel), 0);
    lv_obj_set_style_bg_opa(header, kOpa90, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, lv_color_hex(kClrBorder), 0);
    lv_obj_set_style_pad_all(header, 0, 0);

    int timeWidth = 54;
    if (timeWidth > innerW - 24) {
      timeWidth = innerW - 24;
    }
    if (timeWidth < 18) {
      timeWidth = 18;
    }
    int titleWidth = innerW - timeWidth - 6;
    if (titleWidth < 20) {
      titleWidth = innerW;
    }

    lv_obj_t *titleLabel = lv_label_create(header);
    setSingleLineLabel(titleLabel, titleWidth, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(titleLabel, title.c_str());
    lv_obj_set_style_text_color(titleLabel, lv_color_hex(kClrTextPrimary), 0);
    lv_obj_set_pos(titleLabel, kSidePadding, 4);

    lv_obj_t *timeLabel = lv_label_create(header);
    setSingleLineLabel(timeLabel, timeWidth, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(timeLabel, headerTime.length() > 0 ? headerTime.c_str() : "--:--");
    lv_obj_set_style_text_color(timeLabel, lv_color_hex(kClrTextMuted), 0);
    lv_obj_set_pos(timeLabel, frameW - kSidePadding - timeWidth, 4);

    int y = 4 + kHeaderHeight + 4;
    if (subtitle.length() > 0) {
      lv_obj_t *sub = lv_obj_create(screen);
      disableScroll(sub);
      lv_obj_remove_style_all(sub);
      lv_obj_set_pos(sub, frameX, y);
      lv_obj_set_size(sub, frameW, kSubtitleHeight);
      lv_obj_set_style_radius(sub, 6, 0);
      lv_obj_set_style_bg_color(sub, lv_color_hex(kClrPanelSoft), 0);
      lv_obj_set_style_bg_opa(sub, kOpa85, 0);
      lv_obj_set_style_border_width(sub, 1, 0);
      lv_obj_set_style_border_color(sub, lv_color_hex(kClrBorder), 0);

      lv_obj_t *subLabel = lv_label_create(sub);
      setSingleLineLabel(subLabel, innerW, LV_TEXT_ALIGN_LEFT);
      lv_label_set_text(subLabel, subtitle.c_str());
      lv_obj_set_style_text_color(subLabel, lv_color_hex(kClrTextMuted), 0);
      int subLabelY = (kSubtitleHeight - static_cast<int>(font()->line_height + 2)) / 2;
      if (subLabelY < 0) {
        subLabelY = 0;
      }
      lv_obj_set_pos(subLabel, kSidePadding, subLabelY);
      y += kSubtitleHeight + 4;
    }

    int footerY = h - 6;
    if (footer.length() > 0) {
      footerY = h - kFooterHeight - 4;
      lv_obj_t *foot = lv_obj_create(screen);
      disableScroll(foot);
      lv_obj_remove_style_all(foot);
      lv_obj_set_pos(foot, frameX, footerY);
      lv_obj_set_size(foot, frameW, kFooterHeight);
      lv_obj_set_style_radius(foot, 6, 0);
      lv_obj_set_style_bg_color(foot, lv_color_hex(kClrPanelSoft), 0);
      lv_obj_set_style_bg_opa(foot, kOpa85, 0);
      lv_obj_set_style_border_width(foot, 1, 0);
      lv_obj_set_style_border_color(foot, lv_color_hex(kClrBorder), 0);

      lv_obj_t *footLabel = lv_label_create(foot);
      setSingleLineLabel(footLabel, innerW, LV_TEXT_ALIGN_CENTER);
      lv_label_set_text(footLabel, footer.c_str());
      lv_obj_set_style_text_color(footLabel, lv_color_hex(kClrTextMuted), 0);
      int footLabelY = (kFooterHeight - static_cast<int>(font()->line_height + 2)) / 2;
      if (footLabelY < 0) {
        footLabelY = 0;
      }
      lv_obj_set_pos(footLabel, kSidePadding, footLabelY);
    }

    contentTop = y + 2;
    contentBottom = footer.length() > 0 ? (footerY - 4) : (h - 6);
    if (contentBottom > h - 6) {
      contentBottom = h - 6;
    }
    if (contentBottom < contentTop + kMinContentHeight) {
      contentBottom = contentTop + kMinContentHeight;
      if (contentBottom > h - 6) {
        contentBottom = h - 6;
      }
    }
    if (contentBottom < contentTop) {
      contentBottom = contentTop;
    }
  }

  void renderMenu(const String &title,
                  const std::vector<String> &items,
                  int selected,
                  const String &subtitle,
                  const String &footer) {
    int contentTop = 0;
    int contentBottom = 0;
    renderBase(title, subtitle, footer, contentTop, contentBottom);

    const int w = lv_display_get_horizontal_resolution(port.display());
    int usableHeight = contentBottom - contentTop + 1;
    if (usableHeight < 1) {
      usableHeight = 1;
    }
    int rowHeight = kRowHeight;
    if (usableHeight < rowHeight) {
      rowHeight = usableHeight;
    }
    if (rowHeight < 18 && usableHeight >= 18) {
      rowHeight = 18;
    }

    int maxRows = usableHeight / rowHeight;
    if (maxRows < 1) {
      maxRows = 1;
    }

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
      const int y = contentTop + row * rowHeight;
      if (index < 0 || index >= static_cast<int>(items.size())) {
        continue;
      }

      lv_obj_t *btn = lv_obj_create(lv_screen_active());
      disableScroll(btn);
      lv_obj_remove_style_all(btn);
      const int btnW = w - 20;
      int btnH = rowHeight - 2;
      if (btnH < 1) {
        btnH = 1;
      }
      lv_obj_set_pos(btn, 10, y);
      lv_obj_set_size(btn, btnW, btnH);
      lv_obj_set_style_radius(btn, 8, kStyleAny);
      lv_obj_set_style_border_width(btn, 1, kStyleAny);
      lv_obj_set_style_pad_all(btn, 0, kStyleAny);
      lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, kStyleAny);

      const bool isSelected = index == selected;
      lv_obj_set_style_bg_color(btn,
                                isSelected ? lv_color_hex(kClrAccentSoft) : lv_color_hex(kClrPanel),
                                kStyleAny);
      lv_obj_set_style_border_color(btn,
                                    isSelected ? lv_color_hex(kClrAccent) : lv_color_hex(kClrBorder),
                                kStyleAny);

      lv_obj_t *label = lv_label_create(btn);
      setSingleLineLabel(label, btnW - 14, LV_TEXT_ALIGN_LEFT);
      lv_label_set_text(label, items[static_cast<size_t>(index)].c_str());
      lv_obj_set_style_text_color(label,
                                  isSelected ? lv_color_hex(kClrTextPrimary)
                                             : lv_color_hex(kClrTextPrimary),
                                  kStyleAny);
      lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);

      if (isSelected) {
        lv_obj_t *marker = lv_obj_create(btn);
        disableScroll(marker);
        lv_obj_remove_style_all(marker);
        lv_obj_set_size(marker, 3, btnH - 8);
        lv_obj_set_pos(marker, 4, 4);
        lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(marker, lv_color_hex(kClrAccent), 0);
        lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, 0);
      }
    }

    service(nullptr);
  }

  void renderLauncher(const String &title,
                      const std::vector<String> &items,
                      int selected) {
    updateHeaderIndicators();
    updateSdPercent();

    lv_obj_t *screen = lv_screen_active();
    clearProgressHandles();
    lv_obj_clean(screen);
    disableScroll(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(kLauncherBg), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(kLauncherPrimary), 0);
    lv_obj_set_style_text_opa(screen, LV_OPA_COVER, 0);
    setLabelFont(screen);

    const int w = lv_display_get_horizontal_resolution(port.display());
    const int h = lv_display_get_vertical_resolution(port.display());
    const int count = static_cast<int>(items.size());
    if (count <= 0) {
      lv_obj_t *emptyLabel = lv_label_create(screen);
      setSingleLineLabel(emptyLabel, w - 12, LV_TEXT_ALIGN_CENTER);
      lv_label_set_text(emptyLabel, "No apps");
      lv_obj_set_style_text_color(emptyLabel, lv_color_hex(kLauncherMuted), 0);
      lv_obj_align(emptyLabel, LV_ALIGN_CENTER, 0, 0);
      service(nullptr);
      return;
    }

    const int safeSelected = wrapIndex(selected, count);
    const int prevIndex = wrapIndex(safeSelected - 1, count);
    const int nextIndex = wrapIndex(safeSelected + 1, count);

    const int topX = 4;
    const int topY = 4;
    const int topW = w - (topX * 2);
    const int topH = kHeaderHeight;

    lv_obj_t *topBar = lv_obj_create(screen);
    disableScroll(topBar);
    lv_obj_remove_style_all(topBar);
    lv_obj_set_pos(topBar, topX, topY);
    lv_obj_set_size(topBar, topW, topH);
    lv_obj_set_style_radius(topBar, 8, 0);
    lv_obj_set_style_bg_color(topBar, lv_color_hex(kLauncherBg), 0);
    lv_obj_set_style_bg_opa(topBar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(topBar, 1, 0);
    lv_obj_set_style_border_color(topBar, lv_color_hex(kLauncherLine), 0);

    constexpr int kBatteryBodyW = 18;
    constexpr int kBatteryCapW = 2;
    constexpr int kBatteryIconW = kBatteryBodyW + kBatteryCapW;
    constexpr int kBatteryIconH = 9;
    const int batteryX = topW - kSidePadding - kBatteryIconW;
    const int batteryY = (topH - kBatteryIconH) / 2;
    const int sdLabelW = 56;
    const int sdBatteryGap = 12;
    const int sdX = batteryX - sdBatteryGap - sdLabelW;
    const int titleX = kSidePadding;
    int titleW = sdX - titleX - 6;
    if (titleW < 16) {
      titleW = 16;
    }
    int labelY = (topH - static_cast<int>(font()->line_height + 2)) / 2;
    if (labelY < 0) {
      labelY = 0;
    }
    lv_obj_t *titleLabel = lv_label_create(topBar);
    setSingleLineLabel(titleLabel, titleW > 10 ? titleW : 10, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(titleLabel, ellipsize(title, 18).c_str());
    lv_obj_set_style_text_color(titleLabel, lv_color_hex(kLauncherPrimary), 0);
    lv_obj_set_pos(titleLabel, titleX, labelY);

    String sdText = "SD ";
    if (sdPct >= 0) {
      sdText += String(sdPct);
      sdText += "%";
    } else {
      sdText += "--%";
    }

    lv_obj_t *sdLabel = lv_label_create(topBar);
    setSingleLineLabel(sdLabel, sdLabelW, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_text(sdLabel, sdText.c_str());
    lv_obj_set_style_text_color(sdLabel, lv_color_hex(kLauncherPrimary), 0);
    lv_obj_set_pos(sdLabel, sdX, labelY);

    drawBatteryIcon(topBar, batteryX, batteryY);

    const String selectedName = ellipsize(items[static_cast<size_t>(safeSelected)], 18);
    const String prevName = ellipsize(items[static_cast<size_t>(prevIndex)], 10);
    const String nextName = ellipsize(items[static_cast<size_t>(nextIndex)], 10);

    constexpr int kMainIconOffsetY = -6;
    constexpr int kMainIconBaseH = 46;
    constexpr int kMainIconScale = 384;  // 1.5x (256 == 1.0x)
    bool iconDrawn = launcherIconsAvailable && launcherIconsReady();
    if (iconDrawn) {
      const LauncherIconId selectedIcon = iconIdFromLauncherIndex(safeSelected);
      const LauncherIconId prevIcon = iconIdFromLauncherIndex(prevIndex);
      const LauncherIconId nextIcon = iconIdFromLauncherIndex(nextIndex);

      const lv_image_dsc_t *mainIcon = getLauncherIcon(selectedIcon, LauncherIconVariant::Main);
      const lv_image_dsc_t *leftIcon = getLauncherIcon(prevIcon, LauncherIconVariant::Side);
      const lv_image_dsc_t *rightIcon = getLauncherIcon(nextIcon, LauncherIconVariant::Side);
      if (!mainIcon || !leftIcon || !rightIcon) {
        iconDrawn = false;
      } else {
        lv_obj_t *centerImg = lv_image_create(screen);
        disableScroll(centerImg);
        lv_obj_set_style_bg_opa(centerImg, LV_OPA_TRANSP, 0);
        lv_image_set_src(centerImg, mainIcon);
        lv_obj_set_style_image_recolor(centerImg, lv_color_hex(kLauncherPrimary), 0);
        lv_obj_set_style_image_recolor_opa(centerImg, LV_OPA_COVER, 0);
        lv_image_set_scale(centerImg, kMainIconScale);
        lv_obj_align(centerImg, LV_ALIGN_CENTER, 0, kMainIconOffsetY);

        lv_obj_t *leftImg = lv_image_create(screen);
        disableScroll(leftImg);
        lv_obj_set_style_bg_opa(leftImg, LV_OPA_TRANSP, 0);
        lv_image_set_src(leftImg, leftIcon);
        lv_obj_set_style_image_recolor(leftImg, lv_color_hex(kLauncherSide), 0);
        lv_obj_set_style_image_recolor_opa(leftImg, LV_OPA_COVER, 0);
        lv_image_set_scale(leftImg, kMainIconScale);
        lv_obj_align(leftImg, LV_ALIGN_CENTER, -92, kMainIconOffsetY);

        lv_obj_t *rightImg = lv_image_create(screen);
        disableScroll(rightImg);
        lv_obj_set_style_bg_opa(rightImg, LV_OPA_TRANSP, 0);
        lv_image_set_src(rightImg, rightIcon);
        lv_obj_set_style_image_recolor(rightImg, lv_color_hex(kLauncherSide), 0);
        lv_obj_set_style_image_recolor_opa(rightImg, LV_OPA_COVER, 0);
        lv_image_set_scale(rightImg, kMainIconScale);
        lv_obj_align(rightImg, LV_ALIGN_CENTER, 92, kMainIconOffsetY);
      }
    }

    if (!iconDrawn) {
      lv_obj_t *fallback = lv_label_create(screen);
      setSingleLineLabel(fallback, w - 16, LV_TEXT_ALIGN_CENTER);
      lv_label_set_text(fallback, selectedName.c_str());
      lv_obj_set_style_text_color(fallback, lv_color_hex(kLauncherPrimary), 0);
      lv_obj_align(fallback, LV_ALIGN_CENTER, 0, -6);

      lv_obj_t *sideNames = lv_label_create(screen);
      setSingleLineLabel(sideNames, w - 16, LV_TEXT_ALIGN_CENTER);
      lv_label_set_text(sideNames, (prevName + "   |   " + nextName).c_str());
      lv_obj_set_style_text_color(sideNames, lv_color_hex(kLauncherMuted), 0);
      lv_obj_align(sideNames, LV_ALIGN_CENTER, 0, 16);
    }

    lv_obj_t *nameLabel = lv_label_create(screen);
    prepareLabel(nameLabel);
    setLabelFont(nameLabel);
    lv_label_set_long_mode(nameLabel, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(nameLabel, LV_SIZE_CONTENT);
    lv_obj_set_height(nameLabel, static_cast<int32_t>(font()->line_height + 2));
    lv_label_set_text(nameLabel, selectedName.c_str());
    lv_obj_set_style_text_color(nameLabel, lv_color_hex(kLauncherMuted), 0);
    int nameY = (h / 2) + 42;
    if (iconDrawn) {
      const int scaledMainIconH = (kMainIconBaseH * kMainIconScale + 128) / 256;
      const int iconBottom = (h / 2) + kMainIconOffsetY + (scaledMainIconH / 2);
      nameY = iconBottom + 4;
    }
    if (nameY > h - 16) {
      nameY = h - 16;
    }
    lv_obj_align(nameLabel, LV_ALIGN_TOP_MID, 0, nameY);

    service(nullptr);
  }

  void renderInfo(const String &title,
                  const std::vector<String> &lines,
                  int start,
                  const String &footer) {
    int contentTop = 0;
    int contentBottom = 0;
    renderBase(title, "", footer, contentTop, contentBottom);

    const int w = lv_display_get_horizontal_resolution(port.display());
    int usableHeight = contentBottom - contentTop + 1;
    if (usableHeight < 1) {
      usableHeight = 1;
    }
    int rowHeight = kRowHeight;
    if (usableHeight < rowHeight) {
      rowHeight = usableHeight;
    }
    if (rowHeight < 18 && usableHeight >= 18) {
      rowHeight = 18;
    }

    int maxRows = usableHeight / rowHeight;
    if (maxRows < 1) {
      maxRows = 1;
    }

    for (int row = 0; row < maxRows; ++row) {
      const int lineIndex = start + row;
      const int y = contentTop + row * rowHeight;
      if (lineIndex < 0 || lineIndex >= static_cast<int>(lines.size())) {
        continue;
      }

      lv_obj_t *holder = lv_obj_create(lv_screen_active());
      disableScroll(holder);
      lv_obj_remove_style_all(holder);
      const int holderW = w - 20;
      int holderH = rowHeight - 1;
      if (holderH < 1) {
        holderH = 1;
      }
      lv_obj_set_pos(holder, 10, y);
      lv_obj_set_size(holder, holderW, holderH);
      lv_obj_set_style_bg_color(holder, lv_color_hex(kClrPanel), kStyleAny);
      lv_obj_set_style_bg_opa(holder, kOpa92, kStyleAny);
      lv_obj_set_style_border_width(holder, 1, kStyleAny);
      lv_obj_set_style_border_color(holder, lv_color_hex(kClrBorder), kStyleAny);
      lv_obj_set_style_radius(holder, 8, kStyleAny);
      lv_obj_set_style_pad_all(holder, 0, kStyleAny);

      lv_obj_t *label = lv_label_create(holder);
      setSingleLineLabel(label, holderW - 14, LV_TEXT_ALIGN_LEFT);
      lv_label_set_text(label, lines[static_cast<size_t>(lineIndex)].c_str());
      lv_obj_set_style_text_color(label, lv_color_hex(kClrTextPrimary), kStyleAny);
      lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
    }

    service(nullptr);
  }

  void renderToast(const String &title,
                   const String &message,
                   const String &footer) {
    int contentTop = 0;
    int contentBottom = 0;
    renderBase(title, "", footer, contentTop, contentBottom);

    const int w = lv_display_get_horizontal_resolution(port.display());
    int areaH = contentBottom - contentTop + 1;
    if (areaH < 1) {
      areaH = 1;
    }

    lv_obj_t *box = lv_obj_create(lv_screen_active());
    disableScroll(box);
    lv_obj_remove_style_all(box);
    int boxW = w - 16;
    if (boxW < 80) {
      boxW = w - 4;
    }
    int boxH = areaH - 8;
    if (boxH < 24) {
      boxH = areaH;
    }
    int boxY = contentTop + (areaH - boxH) / 2;
    lv_obj_set_size(box, boxW, boxH);
    lv_obj_set_pos(box, (w - boxW) / 2, boxY);
    lv_obj_set_style_bg_color(box, lv_color_hex(kClrPanel), kStyleAny);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, kStyleAny);
    lv_obj_set_style_border_color(box, lv_color_hex(kClrAccent), kStyleAny);
    lv_obj_set_style_border_width(box, 1, kStyleAny);
    lv_obj_set_style_radius(box, 10, kStyleAny);
    lv_obj_set_style_pad_all(box, 6, kStyleAny);

    lv_obj_t *label = lv_label_create(box);
    setWrapLabel(label, boxW - 18, boxH - 14);
    lv_label_set_text(label, message.c_str());
    lv_obj_set_style_text_color(label, lv_color_hex(kClrTextPrimary), 0);
    lv_obj_center(label);

    service(nullptr);
  }

  void renderTextInput(const String &title,
                       const String &preview,
                       const std::vector<String> &keyLabels,
                       int selected,
                       int selectedCapsIndex,
                       const std::vector<lv_area_t> &areas) {
    int contentTop = 0;
    int contentBottom = 0;
    renderBase(title,
               preview,
               "ROTATE Move   OK Type   BACK",
               contentTop,
               contentBottom);

    const size_t keyCount = keyLabels.size();
    for (size_t i = 0; i < keyCount; ++i) {
      const lv_area_t &a = areas[i];
      lv_obj_t *btn = lv_button_create(lv_screen_active());
      disableScroll(btn);
      lv_obj_remove_style_all(btn);
      lv_obj_set_pos(btn, a.x1, a.y1);
      lv_obj_set_size(btn,
                      static_cast<int32_t>(a.x2 - a.x1 + 1),
                      static_cast<int32_t>(a.y2 - a.y1 + 1));

      bool isSelected = selected == static_cast<int>(i);
      bool isCapsActive = selectedCapsIndex == static_cast<int>(i);

      lv_color_t bg = lv_color_hex(kClrPanel);
      lv_color_t fg = lv_color_hex(kClrTextPrimary);

      if (isCapsActive) {
        bg = lv_color_hex(0x2A4F8C);
        fg = lv_color_hex(kClrTextPrimary);
      }
      if (isSelected) {
        bg = lv_color_hex(kClrAccentSoft);
        fg = lv_color_hex(kClrTextPrimary);
      }

      lv_obj_set_style_bg_color(btn, bg, kStyleAny);
      lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, kStyleAny);
      lv_obj_set_style_border_width(btn, 1, kStyleAny);
      lv_obj_set_style_border_color(btn,
                                    isSelected ? lv_color_hex(kClrAccent) : lv_color_hex(kClrBorder),
                                    kStyleAny);
      lv_obj_set_style_radius(btn, 4, kStyleAny);
      lv_obj_set_style_pad_all(btn, 0, kStyleAny);

      lv_obj_t *label = lv_label_create(btn);
      setSingleLineLabel(label,
                         static_cast<int>(a.x2 - a.x1),
                         LV_TEXT_ALIGN_CENTER);
      lv_label_set_text(label, keyLabels[i].c_str());
      lv_obj_set_style_text_color(label, fg, kStyleAny);
      lv_obj_center(label);
    }

    service(nullptr);
  }

  void renderProgressOverlay(const String &title,
                             const String &message,
                             int percent) {
    lv_obj_t *screen = lv_screen_active();
    const int w = lv_display_get_horizontal_resolution(port.display());
    const int h = lv_display_get_vertical_resolution(port.display());

    int panelW = w - 20;
    if (panelW > 300) {
      panelW = 300;
    }
    if (panelW < 120) {
      panelW = w - 8;
    }
    if (panelW < 80) {
      panelW = w;
    }

    int panelH = h - 24;
    if (panelH > 118) {
      panelH = 118;
    }
    if (panelH < 72) {
      panelH = h - 6;
    }
    if (panelH < 48) {
      panelH = 48;
    }

    const int innerPad = 10;
    const int titleY = 8;
    const int spinnerSize = 22;
    const int messageY = 34;
    const int barY = panelH - 22;
    int messageHeight = panelH - messageY - 16;
    if (messageHeight < 12) {
      messageHeight = 12;
    }
    if (percent >= 0) {
      messageHeight = barY - messageY - 6;
      if (messageHeight < 12) {
        messageHeight = 12;
      }
    }

    const bool needsCreate =
        progressOverlay == nullptr ||
        !lv_obj_is_valid(progressOverlay) ||
        lv_obj_get_parent(progressOverlay) != screen;

    if (needsCreate) {
      clearProgressHandles();

      progressOverlay = lv_obj_create(screen);
      disableScroll(progressOverlay);
      lv_obj_remove_style_all(progressOverlay);
      lv_obj_set_style_bg_color(progressOverlay, lv_color_black(), 0);
      lv_obj_set_style_bg_opa(progressOverlay, kOpa75, 0);
      lv_obj_set_style_border_width(progressOverlay, 0, 0);
      lv_obj_set_style_radius(progressOverlay, 0, 0);
      lv_obj_move_foreground(progressOverlay);

      progressPanel = lv_obj_create(progressOverlay);
      disableScroll(progressPanel);
      lv_obj_remove_style_all(progressPanel);
      lv_obj_set_style_bg_color(progressPanel, lv_color_hex(kClrPanel), kStyleAny);
      lv_obj_set_style_bg_opa(progressPanel, LV_OPA_COVER, kStyleAny);
      lv_obj_set_style_border_color(progressPanel, lv_color_hex(kClrAccent), kStyleAny);
      lv_obj_set_style_border_width(progressPanel, 1, kStyleAny);
      lv_obj_set_style_radius(progressPanel, 10, kStyleAny);
      lv_obj_set_style_pad_all(progressPanel, 0, kStyleAny);

      progressTitle = lv_label_create(progressPanel);
      setSingleLineLabel(progressTitle, panelW - 56, LV_TEXT_ALIGN_LEFT);
      lv_obj_set_style_text_color(progressTitle, lv_color_white(), 0);

      progressSpinner = lv_spinner_create(progressPanel);

      progressMessage = lv_label_create(progressPanel);
      setWrapLabel(progressMessage, panelW - (innerPad * 2), messageHeight);
      lv_obj_set_style_text_color(progressMessage, lv_color_white(), 0);

      progressBar = lv_bar_create(progressPanel);
      lv_bar_set_range(progressBar, 0, 100);
      lv_obj_set_style_bg_color(progressBar, lv_color_hex(kClrPanelSoft), 0);
      lv_obj_set_style_bg_color(progressBar, lv_color_hex(kClrAccent), LV_PART_INDICATOR);

      progressPercent = lv_label_create(progressPanel);
      setSingleLineLabel(progressPercent, 44, LV_TEXT_ALIGN_RIGHT);
      lv_obj_set_style_text_color(progressPercent, lv_color_hex(0xA5E8FF), 0);
    }

    if (progressOverlay && lv_obj_is_valid(progressOverlay)) {
      lv_obj_set_size(progressOverlay, w, h);
      lv_obj_set_pos(progressOverlay, 0, 0);
    }
    if (progressPanel && lv_obj_is_valid(progressPanel)) {
      lv_obj_set_size(progressPanel, panelW, panelH);
      lv_obj_center(progressPanel);
    }
    if (progressTitle && lv_obj_is_valid(progressTitle)) {
      lv_obj_set_width(progressTitle, panelW - 56);
      lv_obj_set_pos(progressTitle, innerPad, titleY);
    }
    if (progressSpinner && lv_obj_is_valid(progressSpinner)) {
      lv_obj_set_size(progressSpinner, spinnerSize, spinnerSize);
      lv_obj_set_pos(progressSpinner, panelW - innerPad - spinnerSize, 6);
    }
    if (progressMessage && lv_obj_is_valid(progressMessage)) {
      lv_obj_set_width(progressMessage, panelW - (innerPad * 2));
      lv_obj_set_height(progressMessage, messageHeight);
      lv_obj_set_pos(progressMessage, innerPad, messageY);
    }
    if (progressBar && lv_obj_is_valid(progressBar)) {
      lv_obj_set_size(progressBar, panelW - (innerPad * 2), 10);
      lv_obj_set_pos(progressBar, innerPad, barY);
    }
    if (progressPercent && lv_obj_is_valid(progressPercent)) {
      lv_obj_set_pos(progressPercent, panelW - innerPad - 44, barY - 16);
    }

    if (progressTitle) {
      lv_label_set_text(progressTitle, title.c_str());
    }
    if (progressMessage) {
      lv_label_set_text(progressMessage, message.c_str());
    }

    if (progressBar && progressPercent) {
      if (percent < 0) {
        lv_obj_add_flag(progressBar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(progressPercent, LV_OBJ_FLAG_HIDDEN);
      } else {
        if (percent > 100) {
          percent = 100;
        }
        lv_obj_clear_flag(progressBar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(progressPercent, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(progressBar, percent, LV_ANIM_OFF);

        char pct[12];
        snprintf(pct, sizeof(pct), "%d%%", percent);
        lv_label_set_text(progressPercent, pct);
      }
    }

    service(nullptr);
  }

  void hideProgressOverlay() {
    if (progressOverlay && lv_obj_is_valid(progressOverlay)) {
      lv_obj_del(progressOverlay);
    }
    clearProgressHandles();
    service(nullptr);
  }
};

UiRuntime::UiRuntime() : impl_(new Impl()) {}

void UiRuntime::begin() {
  if (!impl_->begin()) {
    Serial.println("[ui] runtime begin failed");
    return;
  }

  impl_->service(nullptr);

  int contentTop = 0;
  int contentBottom = 0;
  impl_->renderBase("Boot", "", "", contentTop, contentBottom);

  lv_obj_t *label = lv_label_create(lv_screen_active());
  impl_->setSingleLineLabel(label, 120, LV_TEXT_ALIGN_CENTER);
  lv_label_set_text(label, "Booting...");
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

  impl_->service(nullptr);
  delay(40);
  impl_->service(nullptr);
}

void UiRuntime::tick() {
  impl_->service(nullptr);
}

UiEvent UiRuntime::pollInput() {
  return impl_->pollInput();
}

void UiRuntime::setStatusLine(const String &line) {
  impl_->statusLine = line;
}

void UiRuntime::setLanguage(UiLanguage language) {
  impl_->language = language;
}

UiLanguage UiRuntime::language() const {
  return impl_->language;
}

void UiRuntime::setTimezone(const String &tz) {
  impl_->setTimezone(tz);
}

String UiRuntime::timezone() const {
  return impl_->timezone();
}

bool UiRuntime::syncTimezoneFromIp(String *resolvedTz, String *error) {
  return impl_->syncTimezoneFromIp(resolvedTz, error);
}

int UiRuntime::launcherLoop(const String &title,
                            const std::vector<String> &items,
                            int selectedIndex,
                            const std::function<void()> &backgroundTick) {
  if (items.empty()) {
    return -1;
  }

  int selected = wrapIndex(selectedIndex, static_cast<int>(items.size()));
  bool redraw = true;
  unsigned long lastRefreshMs = millis();

  while (true) {
    const unsigned long now = millis();
    if (redraw || now - lastRefreshMs >= kHeaderRefreshMs) {
      impl_->renderLauncher(title, items, selected);
      redraw = false;
      lastRefreshMs = now;
    }

    impl_->service(&backgroundTick);
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

    delay(10);
  }
}

int UiRuntime::menuLoop(const String &title,
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
    if (redraw || now - lastRefreshMs >= kHeaderRefreshMs) {
      impl_->renderMenu(title, items, selected, subtitle, footer);
      redraw = false;
      lastRefreshMs = now;
    }

    impl_->service(&backgroundTick);
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

    delay(10);
  }
}

void UiRuntime::showInfo(const String &title,
                         const std::vector<String> &lines,
                         const std::function<void()> &backgroundTick,
                         const String &footer) {
  int startIndex = 0;
  bool redraw = true;
  unsigned long lastRefreshMs = millis();

  while (true) {
    const unsigned long now = millis();
    if (redraw || now - lastRefreshMs >= kHeaderRefreshMs) {
      impl_->renderInfo(title, lines, startIndex, footer);
      redraw = false;
      lastRefreshMs = now;
    }

    impl_->service(&backgroundTick);
    UiEvent ev = pollInput();

    if (ev.delta != 0) {
      int next = startIndex + (ev.delta > 0 ? 1 : -1);
      if (next < 0) {
        next = 0;
      }
      if (next > static_cast<int>(lines.size()) - 1) {
        next = static_cast<int>(lines.size()) - 1;
      }
      if (next < 0) {
        next = 0;
      }
      if (next != startIndex) {
        startIndex = next;
        redraw = true;
      }
    }

    if (ev.ok || ev.back) {
      return;
    }

    delay(10);
  }
}

bool UiRuntime::confirm(const String &title,
                        const String &message,
                        const std::function<void()> &backgroundTick,
                        const String &confirmLabel,
                        const String &cancelLabel) {
  std::vector<String> options;
  options.push_back(confirmLabel);
  options.push_back(cancelLabel);
  const int selected = menuLoop(title,
                                options,
                                1,
                                backgroundTick,
                                "OK   BACK",
                                message);
  return selected == 0;
}

bool UiRuntime::textInput(const String &title,
                          String &inOutValue,
                          bool mask,
                          const std::function<void()> &backgroundTick) {
  struct CharKeyPair {
    char normal;
    char shifted;
  };

  enum class KeyAction : uint8_t {
    Character,
    Done,
    Caps,
    Del,
    Space,
    Cancel,
  };

  struct KeySlot {
    KeyAction action = KeyAction::Character;
    char normal = 0;
    char shifted = 0;
    const char *label = "";
    lv_area_t area{};
  };

  static const CharKeyPair kRow0[] = {
      {'1', '!'}, {'2', '@'}, {'3', '#'}, {'4', '$'}, {'5', '%'}, {'6', '^'},
      {'7', '&'}, {'8', '*'}, {'9', '('}, {'0', ')'}, {'-', '_'}, {'=', '+'}};
  static const CharKeyPair kRow1[] = {
      {'q', 'Q'}, {'w', 'W'}, {'e', 'E'}, {'r', 'R'}, {'t', 'T'}, {'y', 'Y'},
      {'u', 'U'}, {'i', 'I'}, {'o', 'O'}, {'p', 'P'}, {'[', '{'}, {']', '}'}};
  static const CharKeyPair kRow2[] = {
      {'a', 'A'}, {'s', 'S'}, {'d', 'D'}, {'f', 'F'}, {'g', 'G'}, {'h', 'H'},
      {'j', 'J'}, {'k', 'K'}, {'l', 'L'}, {';', ':'}, {'\'', '"'}, {'\\', '|'}};
  static const CharKeyPair kRow3[] = {
      {'z', 'Z'}, {'x', 'X'}, {'c', 'C'}, {'v', 'V'}, {'b', 'B'},
      {'n', 'N'}, {'m', 'M'}, {',', '<'}, {'.', '>'}, {'/', '?'}};

  String working = inOutValue;
  bool caps = false;
  int selected = 0;
  bool redraw = true;
  unsigned long lastRefreshMs = millis();

  const int displayWidth = lv_display_get_horizontal_resolution(impl_->port.display());
  const int displayHeight = lv_display_get_vertical_resolution(impl_->port.display());
  const int maxColumns = 12;
  const int keyGap = displayWidth >= 260 ? 2 : 1;
  int keyWidth = (displayWidth - 8 - (keyGap * (maxColumns - 1))) / maxColumns;
  if (keyWidth < 10) {
    keyWidth = 10;
  }

  int fullRowWidth = maxColumns * keyWidth + (maxColumns - 1) * keyGap;
  if (fullRowWidth > displayWidth - 4) {
    keyWidth = (displayWidth - 4 - (keyGap * (maxColumns - 1))) / maxColumns;
    if (keyWidth < 8) {
      keyWidth = 8;
    }
    fullRowWidth = maxColumns * keyWidth + (maxColumns - 1) * keyGap;
  }

  const int contentTop = kHeaderHeight + kSubtitleHeight + 2;
  int contentBottom = displayHeight - kFooterHeight - 2;
  if (contentBottom <= contentTop) {
    contentBottom = contentTop + 60;
  }
  const int availableHeight = contentBottom - contentTop + 1;
  const int rowCount = 5;
  int keyHeight = (availableHeight - (keyGap * (rowCount - 1))) / rowCount;
  if (keyHeight < 12) {
    keyHeight = 12;
  }
  if (keyHeight > 24) {
    keyHeight = 24;
  }
  const int keyboardHeight = rowCount * keyHeight + (rowCount - 1) * keyGap;
  int keyboardTop = contentTop + (availableHeight - keyboardHeight) / 2;
  if (keyboardTop < contentTop) {
    keyboardTop = contentTop;
  }
  int keyboardLeft = (displayWidth - fullRowWidth) / 2;
  if (keyboardLeft < 2) {
    keyboardLeft = 2;
  }
  const bool compactKeyLabels = keyWidth < 16;

  std::vector<KeySlot> keys;
  keys.reserve(64);

  auto addCharRow = [&](const CharKeyPair *row, size_t len, int rowIndex) {
    const int y = keyboardTop + rowIndex * (keyHeight + keyGap);
    const int rowWidth = static_cast<int>(len) * keyWidth +
                         (static_cast<int>(len) - 1) * keyGap;
    int x = (displayWidth - rowWidth) / 2;
    if (x < 2) {
      x = 2;
    }

    for (size_t i = 0; i < len; ++i) {
      KeySlot slot;
      slot.action = KeyAction::Character;
      slot.normal = row[i].normal;
      slot.shifted = row[i].shifted;
      slot.area.x1 = x;
      slot.area.y1 = y;
      slot.area.x2 = x + keyWidth - 1;
      slot.area.y2 = y + keyHeight - 1;
      keys.push_back(slot);
      x += keyWidth + keyGap;
    }
  };

  addCharRow(kRow0, sizeof(kRow0) / sizeof(kRow0[0]), 0);
  addCharRow(kRow1, sizeof(kRow1) / sizeof(kRow1[0]), 1);
  addCharRow(kRow2, sizeof(kRow2) / sizeof(kRow2[0]), 2);
  addCharRow(kRow3, sizeof(kRow3) / sizeof(kRow3[0]), 3);

  const int actionRowY = keyboardTop + (keyHeight + keyGap) * 4;
  static const int kActionUnits[5] = {2, 2, 2, 4, 2};
  static const KeyAction kActionKinds[5] = {
      KeyAction::Done,
      KeyAction::Caps,
      KeyAction::Del,
      KeyAction::Space,
      KeyAction::Cancel};
  static const char *kActionLabelsWide[5] = {"DONE", "CAPS", "DEL", "SPACE", "CANCEL"};
  static const char *kActionLabelsCompact[5] = {"OK", "CAP", "DEL", "SPC", "ESC"};
  const char *const *actionLabels = compactKeyLabels ? kActionLabelsCompact : kActionLabelsWide;

  int actionX = keyboardLeft;
  int capsIndex = -1;
  for (int i = 0; i < 5; ++i) {
    KeySlot slot;
    slot.action = kActionKinds[i];
    slot.label = actionLabels[i];
    const int width = kActionUnits[i] * keyWidth + (kActionUnits[i] - 1) * keyGap;
    slot.area.x1 = actionX;
    slot.area.y1 = actionRowY;
    slot.area.x2 = actionX + width - 1;
    slot.area.y2 = actionRowY + keyHeight - 1;
    if (slot.action == KeyAction::Caps) {
      capsIndex = static_cast<int>(keys.size());
    }
    keys.push_back(slot);
    actionX += width + keyGap;
  }

  auto buildPreview = [&]() -> String {
    String preview = maskIfNeeded(working, mask);
    if (preview.isEmpty()) {
      preview = "(empty)";
    }

    const size_t kMaxPreviewChars = displayWidth >= 260 ? 40 : 24;
    if (preview.length() > kMaxPreviewChars) {
      const size_t tail = kMaxPreviewChars - 3;
      preview = "..." + preview.substring(preview.length() - tail);
    }
    return preview;
  };

  auto labelForKey = [&](const KeySlot &slot) -> String {
    if (slot.action == KeyAction::Character) {
      return String(caps ? slot.shifted : slot.normal);
    }
    if (slot.action == KeyAction::Caps) {
      if (compactKeyLabels) {
        return caps ? "ON" : "CAP";
      }
      return caps ? "CAPS ON" : "CAPS";
    }
    return String(slot.label);
  };

  while (true) {
    const unsigned long now = millis();
    if (redraw || now - lastRefreshMs >= kHeaderRefreshMs) {
      std::vector<String> labels;
      labels.reserve(keys.size());
      for (std::vector<KeySlot>::const_iterator it = keys.begin();
           it != keys.end();
           ++it) {
        labels.push_back(labelForKey(*it));
      }

      impl_->renderTextInput(title,
                             buildPreview(),
                             labels,
                             selected,
                             (caps && capsIndex >= 0) ? capsIndex : -1,
                             [&]() {
                               std::vector<lv_area_t> areas;
                               areas.reserve(keys.size());
                               for (size_t i = 0; i < keys.size(); ++i) {
                                 areas.push_back(keys[i].area);
                               }
                               return areas;
                             }());
      redraw = false;
      lastRefreshMs = now;
    }

    impl_->service(&backgroundTick);
    UiEvent ev = pollInput();

    if (ev.delta != 0) {
      selected = wrapIndex(selected + (ev.delta > 0 ? 1 : -1),
                           static_cast<int>(keys.size()));
      redraw = true;
    }

    if (ev.back) {
      return false;
    }

    if (ev.ok) {
      const KeySlot &slot = keys[static_cast<size_t>(selected)];
      if (slot.action == KeyAction::Character) {
        working += caps ? slot.shifted : slot.normal;
        redraw = true;
      } else if (slot.action == KeyAction::Done) {
        inOutValue = working;
        return true;
      } else if (slot.action == KeyAction::Caps) {
        caps = !caps;
        redraw = true;
      } else if (slot.action == KeyAction::Del) {
        if (working.length() > 0) {
          working.remove(working.length() - 1);
        }
        redraw = true;
      } else if (slot.action == KeyAction::Space) {
        working += " ";
        redraw = true;
      } else if (slot.action == KeyAction::Cancel) {
        return false;
      }
    }

    delay(10);
  }
}

void UiRuntime::showProgressOverlay(const String &title,
                                    const String &message,
                                    int percent) {
  impl_->renderProgressOverlay(title, message, percent);
}

void UiRuntime::hideProgressOverlay() {
  impl_->hideProgressOverlay();
}

void UiRuntime::showToast(const String &title,
                          const String &message,
                          unsigned long showMs,
                          const std::function<void()> &backgroundTick) {
  const unsigned long start = millis();
  unsigned long lastRefreshMs = 0;

  while (true) {
    const unsigned long now = millis();
    if (lastRefreshMs == 0 || now - lastRefreshMs >= kHeaderRefreshMs) {
      impl_->renderToast(title, message, uiText(language(), UiTextKey::OkBackClose));
      lastRefreshMs = now;
    }

    impl_->service(&backgroundTick);
    UiEvent ev = pollInput();
    if (ev.ok || ev.back || now - start >= showMs) {
      return;
    }

    delay(10);
  }
}
