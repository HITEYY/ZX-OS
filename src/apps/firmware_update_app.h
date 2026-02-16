#pragma once

#include <functional>

#include "app_context.h"

void runFirmwareUpdateApp(AppContext &ctx,
                          const std::function<void()> &backgroundTick);
