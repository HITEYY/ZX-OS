#pragma once

#include <functional>

#include "app_context.h"

void runOpenClawApp(AppContext &ctx,
                    const std::function<void()> &backgroundTick);
