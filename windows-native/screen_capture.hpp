#pragma once
#include "action_context.hpp"
namespace screen_capture {
enum class Area { screen, window };
enum class Destination { file, clipboard };
// Captures only when an explicitly assigned action is invoked.
bool capture(Area area,Destination destination,const ActionContext& context={});
}
