#include "input_adapter.h"

#include "../core/board_pins.h"

namespace {

constexpr uint8_t kPinEncoderA = boardpins::kEncoderA;
constexpr uint8_t kPinEncoderB = boardpins::kEncoderB;
constexpr uint8_t kPinOk = boardpins::kEncoderOk;
constexpr uint8_t kPinBack = boardpins::kEncoderBack;

constexpr unsigned long kDebounceMs = 35UL;
constexpr unsigned long kLongPressMs = 750UL;

}  // namespace

InputAdapter::InputAdapter()
    : encoder_(kPinEncoderA,
               kPinEncoderB,
               RotaryEncoder::LatchMode::TWO03) {}

void InputAdapter::begin(lv_display_t *display) {
  pinMode(kPinOk, INPUT_PULLUP);
  pinMode(kPinBack, INPUT_PULLUP);

  encoder_.setPosition(0);
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
  encoder_.tick();
  const int32_t pos = encoder_.getPosition();
  const int32_t rawDelta = pos - lastEncoderPos_;
  if (rawDelta != 0) {
    const int16_t mapped = static_cast<int16_t>(-rawDelta);
    pendingEncDiff_ = static_cast<int16_t>(pendingEncDiff_ + mapped);
    pendingEvent_.delta += mapped;
    lastEncoderPos_ = pos;
  }

  const unsigned long now = millis();

  const bool okPressed = digitalRead(kPinOk) == LOW;
  if (okPressed && !okPrev_) {
    okPressedAt_ = now;
    okLongFired_ = false;
  }
  if (!okPressed && okPrev_) {
    if (!okLongFired_ && now - okPressedAt_ >= kDebounceMs) {
      pendingEvent_.ok = true;
      enqueueKeyPressRelease(LV_KEY_ENTER);
    }
    okPressedAt_ = 0;
    okLongFired_ = false;
  }
  if (okPressed && !okLongFired_ && okPressedAt_ > 0 &&
      now - okPressedAt_ >= kLongPressMs) {
    pendingEvent_.back = true;
    enqueueKeyPressRelease(LV_KEY_ESC);
    okLongFired_ = true;
  }
  okPrev_ = okPressed;

  const bool backPressed = digitalRead(kPinBack) == LOW;
  if (backPressed && !backPrev_) {
    backPressedAt_ = now;
  }
  if (!backPressed && backPrev_) {
    if (now - backPressedAt_ >= kDebounceMs) {
      pendingEvent_.back = true;
      enqueueKeyPressRelease(LV_KEY_ESC);
    }
    backPressedAt_ = 0;
  }
  backPrev_ = backPressed;
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
