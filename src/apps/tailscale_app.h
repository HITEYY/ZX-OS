#pragma once

#include <functional>

#include "app_context.h"

void runTailscaleApp(AppContext &ctx,
                     const std::function<void()> &backgroundTick);
