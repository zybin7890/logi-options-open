#pragma once

namespace cli_entry {

void init_agent_hooks();
void shutdown_agent_hooks();
void attach_parent_console_if_needed();
int run_cli_main(int argc, wchar_t** argv);

} // namespace cli_entry
