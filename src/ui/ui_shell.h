#pragma once

#include <Arduino.h>

#include <functional>
#include <vector>

struct UiEvent {
  int delta = 0;
  bool ok = false;
  bool back = false;
};

class UIShell {
 public:
  UIShell();

  void begin();
  UiEvent pollInput();

  void setStatusLine(const String &line);

  int menuLoop(const String &title,
               const std::vector<String> &items,
               int selectedIndex,
               const std::function<void()> &backgroundTick,
               const String &footer = "OK Select  BACK Exit",
               const String &subtitle = "");

  void showInfo(const String &title,
                const std::vector<String> &lines,
                const std::function<void()> &backgroundTick,
                const String &footer = "BACK: Exit");

  bool confirm(const String &title,
               const String &message,
               const std::function<void()> &backgroundTick,
               const String &confirmLabel = "Yes",
               const String &cancelLabel = "No");

  bool textInput(const String &title,
                 String &inOutValue,
                 bool mask,
                 const std::function<void()> &backgroundTick);

  void showToast(const String &title,
                 const String &message,
                 unsigned long showMs,
                 const std::function<void()> &backgroundTick);

 private:
  class Impl;
  Impl *impl_;
};
