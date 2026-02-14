#pragma once

#include <functional>

#include "app_context.h"

void runSettingsApp(AppContext &ctx,
                    const std::function<void()> &backgroundTick);
