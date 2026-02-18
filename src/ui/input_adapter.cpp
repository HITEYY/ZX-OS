#include "input_adapter.h"

#include "../core/board_pins.h"
#include "../hal/board_config.h"
#include "user_config.h"

namespace {

#if HAL_HAS_ENCODER
constexpr uint8_t kPinEncoderA = boardpins::kEncoderA;
constexpr uint8_t kPinEncoderB = boardpins::kEncoderB;
#endif

#if defined(HAL_PIN_BTN_OK) && HAL_PIN_BTN_OK >= 0
constexpr uint8_t kPinOk = boardpins::kEncoderOk;
#endif
#if defined(HAL_PIN_BTN_BACK) && HAL_PIN_BTN_BACK >= 0
constexpr uint8_t kPinBack = boardpins::kEncoderBack;
#endif

constexpr unsigned long kDebounceMs = 20UL;
constexpr unsigned long kLongPressMs = 750UL;
constexpr unsigned long kPinRefreshMs = 1000UL;
constexpr unsigned long kTraceHeartbeatMs = 1500UL;

uint8_t saturatingInc(uint8_t value) {
  if (value == 0xFFU) {
    return value;
  }
  return static_cast<uint8_t>(value + 1U);
}

}  // namespace

InputAdapter::InputAdapter()
#if HAL_HAS_ENCODER
    : encoder_(kPinEncoderA,
               kPinEncoderB,
               RotaryEncoder::LatchMode::TWO03) {}
#else
    : encoder_(0, 1, RotaryEncoder::LatchMode::TWO03) {}
#endif

void InputAdapter::begin(lv_display_t *display) {
#if HAL_HAS_ENCODER
  pinMode(kPinEncoderA, INPUT_PULLUP);
  pinMode(kPinEncoderB, INPUT_PULLUP);
#endif
#if defined(HAL_PIN_BTN_OK) && HAL_PIN_BTN_OK >= 0
  pinMode(kPinOk, INPUT_PULLUP);
#endif
#if defined(HAL_PIN_BTN_BACK) && HAL_PIN_BTN_BACK >= 0
  pinMode(kPinBack, INPUT_PULLUP);
#endif
  lastPinRefreshAt_ = millis();
  lastTraceAt_ = 0;
  lastTraceA_ = -1;
  lastTraceB_ = -1;
  lastTraceOk_ = -1;
  lastTraceBack_ = -1;
  lastTracePos_ = 0;
  lastTraceEncDiff_ = 0;
  lastTraceQ_ = 0;

#if HAL_HAS_ENCODER
  encoder_.tick();
  encoder_.setPosition(0);
#endif
  lastEncoderPos_ = 0;
  pendingEncDiff_ = 0;

  indev_ = lv_indev_create();
  lv_indev_set_type(indev_, LV_INDEV_TYPE_ENCODER);
  lv_indev_set_display(indev_, display);
  lv_indev_set_read_cb(indev_, readCb);
  lv_indev_set_user_data(indev_, this);
}

void InputAdapter::setGroup(lv_group_t *group) {
  if (!indev_) {
    return;
  }
  lv_indev_set_group(indev_, group);
}

lv_indev_t *InputAdapter::indev() const {
  return indev_;
}

void InputAdapter::enqueueKey(uint32_t key, lv_indev_state_t state) {
  if (keyCount_ >= kQueueSize) {
    keyHead_ = static_cast<uint8_t>((keyHead_ + 1U) % kQueueSize);
    --keyCount_;
  }

  keyQueue_[keyTail_].key = key;
  keyQueue_[keyTail_].state = state;
  keyTail_ = static_cast<uint8_t>((keyTail_ + 1U) % kQueueSize);
  ++keyCount_;
}

void InputAdapter::enqueueKeyPressRelease(uint32_t key) {
  enqueueKey(key, LV_INDEV_STATE_PRESSED);
  enqueueKey(key, LV_INDEV_STATE_RELEASED);
}

bool InputAdapter::dequeueKey(uint32_t &key, lv_indev_state_t &state) {
  if (keyCount_ == 0) {
    return false;
  }

  key = keyQueue_[keyHead_].key;
  state = keyQueue_[keyHead_].state;
  keyHead_ = static_cast<uint8_t>((keyHead_ + 1U) % kQueueSize);
  --keyCount_;
  return true;
}

void InputAdapter::tick() {
  const unsigned long now = millis();
  if (now - lastPinRefreshAt_ >= kPinRefreshMs) {
#if HAL_HAS_ENCODER
    pinMode(kPinEncoderA, INPUT_PULLUP);
    pinMode(kPinEncoderB, INPUT_PULLUP);
#endif
#if defined(HAL_PIN_BTN_OK) && HAL_PIN_BTN_OK >= 0
    pinMode(kPinOk, INPUT_PULLUP);
#endif
#if defined(HAL_PIN_BTN_BACK) && HAL_PIN_BTN_BACK >= 0
    pinMode(kPinBack, INPUT_PULLUP);
#endif
    lastPinRefreshAt_ = now;
  }

#if HAL_HAS_ENCODER
  encoder_.tick();
  const int32_t pos = encoder_.getPosition();
  const int32_t rawDelta = pos - lastEncoderPos_;
  if (rawDelta != 0) {
    const int16_t mapped = static_cast<int16_t>(-rawDelta);
    pendingEncDiff_ = static_cast<int16_t>(pendingEncDiff_ + mapped);
    pendingEvent_.delta += mapped;
    lastEncoderPos_ = pos;
  }
#elif defined(HAL_HAS_TRACKBALL) && HAL_HAS_TRACKBALL
  // Trackball navigation: read directional pins as scroll events.
  if (digitalRead(boardpins::kTrackballUp) == LOW) {
    pendingEncDiff_ = static_cast<int16_t>(pendingEncDiff_ - 1);
    pendingEvent_.delta -= 1;
  }
  if (digitalRead(boardpins::kTrackballDown) == LOW) {
    pendingEncDiff_ = static_cast<int16_t>(pendingEncDiff_ + 1);
    pendingEvent_.delta += 1;
  }
#endif

  if (!okBackBlocked_) {
#if defined(HAL_PIN_BTN_OK) && HAL_PIN_BTN_OK >= 0
    const bool okPressed = digitalRead(kPinOk) == LOW;
#elif defined(HAL_HAS_TRACKBALL) && HAL_HAS_TRACKBALL
    const bool okPressed = digitalRead(boardpins::kTrackballClick) == LOW;
#else
    const bool okPressed = false;
#endif
    if (okPressed && !okPrev_) {
      okPressedAt_ = now;
      okLongFired_ = false;
    }
    if (!okPressed && okPrev_) {
      if (!okLongFired_ && now - okPressedAt_ >= kDebounceMs) {
        pendingEvent_.ok = true;
        pendingEvent_.okCount = saturatingInc(pendingEvent_.okCount);
        enqueueKeyPressRelease(LV_KEY_ENTER);
      }
      okPressedAt_ = 0;
      okLongFired_ = false;
    }
    if (okPressed && !okLongFired_ && okPressedAt_ > 0 &&
        now - okPressedAt_ >= kLongPressMs) {
      pendingEvent_.back = true;
      pendingEvent_.okLong = true;
      pendingEvent_.backCount = saturatingInc(pendingEvent_.backCount);
      pendingEvent_.okLongCount = saturatingInc(pendingEvent_.okLongCount);
      enqueueKeyPressRelease(LV_KEY_ESC);
      okLongFired_ = true;
    }
    okPrev_ = okPressed;

#if defined(HAL_PIN_BTN_BACK) && HAL_PIN_BTN_BACK >= 0
    const bool backPressed = digitalRead(kPinBack) == LOW;
    if (backPressed && !backPrev_) {
      backPressedAt_ = now;
    }
    if (!backPressed && backPrev_) {
      if (now - backPressedAt_ >= kDebounceMs) {
        pendingEvent_.back = true;
        pendingEvent_.backCount = saturatingInc(pendingEvent_.backCount);
        enqueueKeyPressRelease(LV_KEY_ESC);
      }
      backPressedAt_ = 0;
    }
    backPrev_ = backPressed;
#endif
  } else {
#if defined(HAL_PIN_BTN_OK) && HAL_PIN_BTN_OK >= 0
    okPrev_ = digitalRead(kPinOk) == LOW;
#elif defined(HAL_HAS_TRACKBALL) && HAL_HAS_TRACKBALL
    okPrev_ = digitalRead(boardpins::kTrackballClick) == LOW;
#endif
#if defined(HAL_PIN_BTN_BACK) && HAL_PIN_BTN_BACK >= 0
    backPrev_ = digitalRead(kPinBack) == LOW;
#endif
    okPressedAt_ = 0;
    backPressedAt_ = 0;
    okLongFired_ = false;
  }

#if USER_INPUT_TRACE_ENABLED && HAL_HAS_ENCODER
  const int32_t pos_trace = encoder_.getPosition();
  const int a = digitalRead(kPinEncoderA);
  const int b = digitalRead(kPinEncoderB);
#if defined(HAL_PIN_BTN_OK) && HAL_PIN_BTN_OK >= 0
  const int ok = digitalRead(kPinOk);
#else
  const int ok = 1;
#endif
#if defined(HAL_PIN_BTN_BACK) && HAL_PIN_BTN_BACK >= 0
  const int back = digitalRead(kPinBack);
#else
  const int back = 1;
#endif
  const bool changed = (a != lastTraceA_) ||
                       (b != lastTraceB_) ||
                       (ok != lastTraceOk_) ||
                       (back != lastTraceBack_) ||
                       (pos_trace != lastTracePos_) ||
                       (pendingEncDiff_ != lastTraceEncDiff_) ||
                       (keyCount_ != lastTraceQ_);
  if (changed || lastTraceAt_ == 0 || now - lastTraceAt_ >= kTraceHeartbeatMs) {
    lastTraceAt_ = now;
    Serial.printf("[input] A=%d B=%d OK=%d BACK=%d pos=%ld encDiff=%d q=%u\n",
                  a,
                  b,
                  ok,
                  back,
                  static_cast<long>(pos_trace),
                  static_cast<int>(pendingEncDiff_),
                  static_cast<unsigned int>(keyCount_));
    lastTraceA_ = a;
    lastTraceB_ = b;
    lastTraceOk_ = ok;
    lastTraceBack_ = back;
    lastTracePos_ = pos_trace;
    lastTraceEncDiff_ = pendingEncDiff_;
    lastTraceQ_ = keyCount_;
  }
#endif
}

void InputAdapter::resetState() {
  pendingEvent_ = InputEvent{};
  pendingEncDiff_ = 0;
  keyHead_ = 0;
  keyTail_ = 0;
  keyCount_ = 0;

  const unsigned long now = millis();
#if defined(HAL_PIN_BTN_OK) && HAL_PIN_BTN_OK >= 0
  okPrev_ = digitalRead(kPinOk) == LOW;
#elif defined(HAL_HAS_TRACKBALL) && HAL_HAS_TRACKBALL
  okPrev_ = digitalRead(boardpins::kTrackballClick) == LOW;
#else
  okPrev_ = false;
#endif
#if defined(HAL_PIN_BTN_BACK) && HAL_PIN_BTN_BACK >= 0
  backPrev_ = digitalRead(kPinBack) == LOW;
#else
  backPrev_ = false;
#endif
  okPressedAt_ = okPrev_ ? now : 0;
  backPressedAt_ = backPrev_ ? now : 0;
  okLongFired_ = false;

#if HAL_HAS_ENCODER
  const int32_t pos = encoder_.getPosition();
  lastEncoderPos_ = pos;
#endif
}

void InputAdapter::setOkBackBlocked(bool blocked) {
  okBackBlocked_ = blocked;
  if (!blocked) {
    return;
  }

  pendingEvent_.ok = false;
  pendingEvent_.back = false;
  pendingEvent_.okLong = false;
  pendingEvent_.okCount = 0;
  pendingEvent_.backCount = 0;
  pendingEvent_.okLongCount = 0;
  keyHead_ = 0;
  keyTail_ = 0;
  keyCount_ = 0;
  okPressedAt_ = 0;
  backPressedAt_ = 0;
  okLongFired_ = false;
}

InputEvent InputAdapter::pollEvent() {
  InputEvent out = pendingEvent_;
  pendingEvent_ = InputEvent{};
  return out;
}

void InputAdapter::readCb(lv_indev_t *indev, lv_indev_data_t *data) {
  InputAdapter *self = static_cast<InputAdapter *>(lv_indev_get_user_data(indev));
  if (!self) {
    data->enc_diff = 0;
    data->state = LV_INDEV_STATE_RELEASED;
    data->key = LV_KEY_ENTER;
    return;
  }

  data->enc_diff = self->pendingEncDiff_;
  self->pendingEncDiff_ = 0;

  uint32_t key = LV_KEY_ENTER;
  lv_indev_state_t state = LV_INDEV_STATE_RELEASED;
  if (self->dequeueKey(key, state)) {
    data->key = key;
    data->state = state;
  } else {
    data->key = LV_KEY_ENTER;
    data->state = LV_INDEV_STATE_RELEASED;
  }
}
