# ZX-OS App Development Guide

This guide is for developers who want to add or modify apps in ZX-OS with minimal trial and error.

## 1. Development workflow at a glance

1. Implement app logic in `src/apps/<your_app>.cpp` with a matching header.
2. Use `AppContext` for all shared services and config.
3. Build UX with `UiRuntime` primitives (`menuLoop`, `showInfo`, `showToast`, text/number input, confirm).
4. Keep long operations non-blocking by calling `backgroundTick`.
5. Wire the app into launcher/navigation (or a submenu) in `src/ui/ui_navigator.cpp` or an existing parent app.
6. If settings are edited, set `ctx.configDirty` and persist through `saveConfig(...)` flows.

## 2. Architecture you should follow

### 2.1 Entry points

- Boot starts from `setup()` in `src/main.cpp`.
- App execution repeatedly happens through `gUiNav.runLauncher(...)` in `loop()`.

### 2.2 Shared runtime state

`AppContext` gives each app access to:

- `RuntimeConfig config`
- `WifiManager* wifi`
- `GatewayClient* gateway`
- `BleManager* ble`
- `UiRuntime* uiRuntime`
- `UiNavigator* uiNav`
- `bool configDirty`

Design your app to use this context instead of creating duplicate singleton/global state.

### 2.3 Background tick contract

Many app APIs accept `const std::function<void()> &backgroundTick`.
Call it while waiting on network/filesystem/hardware work to keep:

- UI responsive,
- gateway reconnection alive,
- BLE/Wi-Fi manager state progressing.

## 3. Creating a new app module

## 3.1 File template

Create:

- `src/apps/my_feature_app.h`
- `src/apps/my_feature_app.cpp`

Header pattern:

```cpp
#pragma once

#include <functional>
#include "app_context.h"

void runMyFeatureApp(AppContext &ctx,
                     const std::function<void()> &backgroundTick);
```

### 3.2 Minimal app loop pattern

In `runMyFeatureApp(...)`, use a menu loop:

- Build `std::vector<String> menu`
- Call `ctx.uiRuntime->menuLoop(...)`
- Route by `choice`
- Exit on BACK (`choice < 0` or explicit Back item)

This matches existing apps and keeps interaction consistent.

### 3.3 Wiring into launcher

To make the app visible from home:

1. Add include in `src/ui/ui_navigator.cpp`.
2. Add menu label in `items`.
3. Add `if/else` dispatch to `runMyFeatureApp(...)`.
4. (Optional) extend i18n keys in `src/ui/i18n.h/.cpp` if you need localized names.

## 4. UI patterns and conventions

### 4.1 Reusable UI runtime APIs

Prefer these built-ins:

- `menuLoop(...)`: selectable list screen.
- `launcherLoop(...)`: home grid/list behavior.
- `showInfo(...)`: multiline read-only detail pages.
- `showToast(...)`: short status notifications.
- `textInput(...)`, `numberWheelInput(...)`: data entry.
- `confirm(...)`: destructive/sensitive operation confirmation.

### 4.2 UX consistency guidelines

- Always include a `Back` menu item for clarity.
- Keep toast titles stable (`"Wi-Fi"`, `"APPMarket"`, etc.) for debuggability.
- For risky operations (install/reset/delete), use double confirmation.
- Trim long dynamic values for small display surfaces.

## 5. Working with configuration safely

### 5.1 RuntimeConfig fields you will likely touch

- Connectivity: `wifiSsid`, `wifiPassword`, `gatewayUrl`, auth fields, BLE fields.
- UX: `uiLanguage`, `timezoneTz`, `displayBrightnessPercent`.
- APPMarket: repo/asset fields.

### 5.2 Save/apply sequence

Recommended sequence after edits:

1. Validate settings where applicable.
2. `saveConfig(ctx.config, &err)`.
3. Set `ctx.configDirty = false` on success.
4. Reconfigure dependent services (`wifi/gateway/ble`).
5. Trigger connect/reconnect/disconnect actions as needed.

This mirrors stable behavior already used in OpenClaw and Settings flows.

## 6. Hardware and IO considerations

### 6.1 SPI bus sharing

ZX-OS uses multiple SPI peripherals (TFT, SD, CC1101, others).
Respect CS pin discipline and shared bus helpers to avoid contention.

### 6.2 SD card usage

When your app uses files:

- Ensure SD is mounted before IO.
- Handle mount/open/read failures with user-visible toast messages.
- Prefer deterministic app-owned file paths (e.g., `/appmarket/...`, `/firmware/...`).

### 6.3 Deep sleep awareness

The platform can enter deep sleep via long button hold. Keep app operations resilient to abrupt power-state transitions and avoid assuming infinitely long sessions.

## 7. Networking integration tips

### 7.1 Wi-Fi-dependent features

Before network calls:

- Check Wi-Fi status.
- Provide explicit user feedback when offline.

### 7.2 Gateway-driven features

For gateway message send flows:

- Verify gateway readiness.
- Use retry/reconnect strategy for burst operations.
- Surface `lastError()` context in toast/info screens when possible.

## 8. Testing checklist for app changes

Before opening a PR, verify at least:

1. App opens/exits repeatedly without UI lock.
2. BACK behavior always returns correctly.
3. Any config edit path updates dirty state correctly.
4. Save/apply persists across reboot.
5. Hardware-dependent functions fail gracefully when module is missing.
6. Long operations keep UI responsive (background tick observed).

## 9. Suggested future extension pattern

If you need plugin-like growth:

- Keep app entry signatures uniform.
- Group module-specific helpers in the app `.cpp` anonymous namespace.
- Add a tiny feature doc section in `docs/FEATURES.md` whenever behavior changes.

This keeps discoverability high for the next developer.

