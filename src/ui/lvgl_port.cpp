#include "lvgl_port.h"

#include <esp_heap_caps.h>

#include "../core/board_pins.h"
#include "../core/shared_spi_bus.h"
#include "../hal/board_config.h"

namespace {

constexpr uint16_t kBufferLines = 24;
constexpr uint8_t kBacklightFullDuty = 254;

}  // namespace

LvglPort::LvglPort() = default;

void *LvglPort::allocateBuffer(size_t bytes) {
  void *ptr = nullptr;
#if CONFIG_SPIRAM || BOARD_HAS_PSRAM
  ptr = ps_malloc(bytes);
#endif
  if (!ptr) {
    ptr = heap_caps_malloc(bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  }
  if (!ptr) {
    ptr = malloc(bytes);
  }
  return ptr;
}

bool LvglPort::begin() {
  if (initialized_) {
    return true;
  }

  sharedspi::prepareChipSelects();

  // Backlight can remain off after deep sleep; force it on when LVGL starts.
#if HAL_HAS_DISPLAY
  pinMode(boardpins::kTftBacklight, OUTPUT);
  analogWrite(boardpins::kTftBacklight, kBacklightFullDuty);

  pinMode(boardpins::kTftCs, OUTPUT);
  digitalWrite(boardpins::kTftCs, HIGH);
#endif

  tft_.init();
  sharedspi::adoptInitializedBus(&TFT_eSPI::getSPIinstance());
  tft_.setRotation(HAL_DISPLAY_ROTATION);
  tft_.fillScreen(TFT_BLACK);
  tft_.setSwapBytes(true);
  tft_.setTextColor(TFT_WHITE, TFT_RED);
  tft_.setTextDatum(TL_DATUM);

  auto showFatal = [&](const char *msg) {
    tft_.fillScreen(TFT_RED);
    tft_.drawString(msg, 4, 4, 2);
  };

  lv_init();

  const uint32_t width = static_cast<uint32_t>(tft_.width());
  const uint32_t height = static_cast<uint32_t>(tft_.height());
  const size_t bufPixels = static_cast<size_t>(width) * kBufferLines;
  const size_t bufBytes = bufPixels * sizeof(lv_color_t);

  buf1_ = static_cast<lv_color_t *>(allocateBuffer(bufBytes));
  buf2_ = static_cast<lv_color_t *>(allocateBuffer(bufBytes));
  if (!buf1_) {
    Serial.println("[ui] LVGL draw buffer alloc failed");
    showFatal("LVGL buf1 alloc failed");
    return false;
  }
  if (!buf2_) {
    Serial.println("[ui] LVGL second buffer alloc failed, falling back to single buffer");
  }

  display_ = lv_display_create(static_cast<int32_t>(width), static_cast<int32_t>(height));
  if (!display_) {
    Serial.println("[ui] LVGL display create failed");
    showFatal("LVGL display create failed");
    return false;
  }

  lv_display_set_user_data(display_, this);
  lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(display_, flushCb);
  lv_display_set_buffers(display_,
                         reinterpret_cast<void *>(buf1_),
                         reinterpret_cast<void *>(buf2_),
                         static_cast<uint32_t>(bufBytes),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_default(display_);

  lastTickMs_ = millis();
  initialized_ = true;
  return true;
}

void LvglPort::pump() {
  if (!initialized_) {
    return;
  }

  const uint32_t now = millis();
  uint32_t diff = now - lastTickMs_;
  if (diff > 1000U) {
    diff = 1000U;
  }
  if (diff > 0U) {
    lv_tick_inc(diff);
    lastTickMs_ = now;
  }

  lv_timer_handler();
}

lv_display_t *LvglPort::display() const {
  return display_;
}

TFT_eSPI &LvglPort::tft() {
  return tft_;
}

bool LvglPort::ready() const {
  return initialized_ && display_ != nullptr;
}

void LvglPort::flushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *pxMap) {
  LvglPort *self = static_cast<LvglPort *>(lv_display_get_user_data(disp));
  if (!self) {
    lv_display_flush_ready(disp);
    return;
  }

  const uint32_t width = static_cast<uint32_t>(area->x2 - area->x1 + 1);
  const uint32_t height = static_cast<uint32_t>(area->y2 - area->y1 + 1);

  self->tft_.startWrite();
  self->tft_.setAddrWindow(area->x1, area->y1, width, height);
  self->tft_.pushColors(reinterpret_cast<uint16_t *>(pxMap), width * height, true);
  self->tft_.endWrite();

  lv_display_flush_ready(disp);
}
