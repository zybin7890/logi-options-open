#pragma once
#include "action_context.hpp"

namespace system_actions {
enum class Action { brightness_up, brightness_down, microphone_mute, shutdown, sleep,
                    high_contrast, close_window, this_pc };
// Returns the actual Windows/API acceptance, never a substitute shortcut.
bool perform(Action action,const ActionContext& context={});
}
