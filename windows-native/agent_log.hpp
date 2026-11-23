#pragma once
#include "ui_settings.hpp"
#include <cstdio>

namespace agent_log {
using ui_settings::LogLevel;
// Set once by the GUI entry point before worker threads start. CLI commands
// retain their original output streams regardless of the tray preferences.
inline bool gui_filter{};

inline bool log_enabled(LogLevel level) {
    return !gui_filter || ui_settings::log_level_now()>=level;
}
template<class... Args>
void agent_message(LogLevel level,FILE* cli_stream,const char* format,Args... args) {
    if(!log_enabled(level))return;
    auto* stream=gui_filter?stderr:cli_stream;
    if constexpr(sizeof...(args)==0)std::fputs(format,stream);
    else std::fprintf(stream,format,args...);
}
}
