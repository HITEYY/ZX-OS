#pragma once

#include <functional>

#include "app_context.h"

void runAppMarketApp(AppContext &ctx,
                     const std::function<void()> &backgroundTick);

