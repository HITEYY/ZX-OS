#pragma once

#include <Arduino.h>
#include <lvgl.h>

enum class LauncherIconId : uint8_t {
  AppMarket = 0,
  Settings = 1,
  FileExplorer = 2,
  OpenClaw = 3,
};

enum class LauncherIconVariant : uint8_t {
  Main = 0,
  Side = 1,
};

bool initLauncherIcons();
bool launcherIconsReady();
const lv_image_dsc_t *getLauncherIcon(LauncherIconId id, LauncherIconVariant variant);

