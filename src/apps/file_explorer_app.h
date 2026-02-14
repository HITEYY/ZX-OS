#pragma once

#include <functional>

#include "app_context.h"

void runFileExplorerApp(AppContext &ctx,
                        const std::function<void()> &backgroundTick);
