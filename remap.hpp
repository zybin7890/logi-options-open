#pragma once

int run_agent(const wchar_t* path, bool take_over = false);
int stop_agent();
bool reload_agent(const wchar_t* path);
int preview_ring(const wchar_t* path);
int play_actions(const wchar_t* path, bool validate_only);
bool check_mappings(const wchar_t* path);
bool options_running();
bool agent_running();
void print_agent_status();
bool set_agent_thumb_speed(unsigned speed);
