#pragma once
#include <array>
#include <string_view>

int run_agent(const wchar_t* path, bool take_over = false);
int stop_agent();
bool reload_agent(const wchar_t* path);
using ReloadFingerprint = std::array<unsigned char,32>;
enum class ReloadStatus {
    applied, superseded, read_failed, invalid_mapping, hotkey_failed, hardware_failed,
    device_unavailable, stopped, unsupported, not_running, busy, timeout, cancelled,
    ipc_error, configuration_mismatch
};
struct ReloadResult {
    ReloadStatus status{ReloadStatus::ipc_error};
    unsigned long long request_id{},instance_id{},revision{};
    unsigned long pid{};
    ReloadFingerprint fingerprint{};
};
bool mapping_fingerprint(std::string_view bytes,ReloadFingerprint& result);
// Cancellation/timeout ends only the wait, not a published request's lifetime.
ReloadResult reload_agent_confirmed(const wchar_t* path,unsigned long long revision,
    const ReloadFingerprint& fingerprint,unsigned timeout_ms=4000,void* cancel_event=nullptr);
const char* reload_status_name(ReloadStatus status);
// True only while this incarnation and its last successful commit still match.
bool is_current_reload_result(const ReloadResult& result);
int preview_ring(const wchar_t* path);
int play_actions(const wchar_t* path, bool validate_only);
bool check_mappings(const wchar_t* path);
bool options_running();
bool agent_running();
void print_agent_status();
bool set_agent_thumb_speed(unsigned speed);

enum class DpiCommandStatus {
    applied, save_failed, write_failed, readback_failed, invalid_value,
    device_unavailable, stopped, unsupported, not_running, busy, timeout,
    ipc_error, configuration_mismatch
};
struct DpiCommandResult {
    DpiCommandStatus status{DpiCommandStatus::ipc_error};
    unsigned long long request_id{};
    unsigned requested{},observed{};
    bool observed_valid{},applied{},saved{};
};
// Only not_running permits a caller to use its standalone hardware path.
// A timeout is an unknown outcome, never permission for another HID writer.
DpiCommandResult set_agent_dpi(unsigned dpi,unsigned timeout_ms=4000,
                               const wchar_t* expected_mapping=nullptr);
const char* dpi_command_status_name(DpiCommandStatus status);

enum class SettingKey { smartshift,ratchet,hires,invert,thumb_invert };
enum class SettingStatus {
    applied,save_failed,write_failed,readback_failed,invalid_value,device_unavailable,
    stopped,unsupported,not_running,busy,timeout,cancelled,ipc_error,configuration_mismatch,official_conflict
};
struct SettingResult {
    SettingStatus status{SettingStatus::ipc_error};
    unsigned long long request_id{},instance_id{},revision{};
    unsigned long pid{};
    SettingKey key{};
    unsigned requested{};
    std::array<unsigned,5> observed{};
    unsigned observed_mask{};
    bool applied{},saved{},software{};
};
// Diverted thumb inversion is a logical software direction; software marks this case.
// Acknowledgement also verifies hardware remains diverted with raw inversion disabled.
SettingResult set_agent_setting(SettingKey key,unsigned value,unsigned long long revision=0,
    unsigned timeout_ms=4000,const wchar_t* expected_mapping=nullptr,void* cancel_event=nullptr);
const char* setting_status_name(SettingStatus status);
bool is_current_setting_result(const SettingResult& result);
struct AgentSettingsSnapshot {
    bool available{},ready{},saved{};
    unsigned long pid{};
    unsigned long long instance_id{};
    std::array<unsigned,5> observed{};
    std::array<unsigned long long,5> sampled_at_ms{};
    unsigned observed_mask{},software_mask{};
};
// Cached last verified readback, never a new hardware query. Per-field timestamps
// allow callers to distinguish this logical snapshot from a live device response.
AgentSettingsSnapshot read_agent_settings();

struct AgentStatusSnapshot {
    bool available{};
    unsigned long pid{};
    bool mappings_ready{},settings_ready{},ready{};
};
AgentStatusSnapshot read_agent_status();
