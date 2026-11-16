#pragma once

#include <string_view>

namespace smart_system {
struct Macro {
    std::wstring_view type, object, member, value, code;
};

// Semantic fields from Options+ card_presets_win.json. These use existing
// backend operations; unrecognized original operations must remain rejected.
inline constexpr Macro macros[]{
    {L"SYSTEM", L"system", L"action", L"LOCK", L"Lock"},
    {L"SYSTEM", L"system", L"action", L"MINIMIZE", L"Minimize"},
    {L"SYSTEM", L"system", L"action", L"MAXIMIZE", L"Maximize"},
    {L"SYSTEM", L"system", L"action", L"CLOSE", L"CloseWindow"},
    {L"SYSTEM", L"system", L"action", L"TASK_VIEW", L"Win+Tab"},
    {L"SYSTEM", L"system", L"action", L"WINDOWS_PRINT_SCREEN", L"PrintScreen"},
    {L"SYSTEM", L"system", L"action", L"SCROLL_LEFT", L"ScrollLeft"},
    {L"SYSTEM", L"system", L"action", L"SCROLL_RIGHT", L"ScrollRight"},
    {L"SYSTEM", L"system", L"action", L"ZOOM_IN", L"ZoomIn"},
    {L"SYSTEM", L"system", L"action", L"ZOOM_OUT", L"ZoomOut"},
    {L"SYSTEM", L"system", L"action", L"BRIGHTNESS_UP", L"BrightnessUp"},
    {L"SYSTEM", L"system", L"action", L"BRIGHTNESS_DOWN", L"BrightnessDown"},
    {L"SYSTEM", L"system", L"action", L"SHUT_DOWN", L"Shutdown"},
    {L"SYSTEM", L"system", L"action", L"SLEEP", L"Sleep"},
    {L"SYSTEM", L"system", L"action", L"WINDOWS_HIGH_CONTRAST", L"HighContrast"},
    {L"SYSTEM", L"system", L"action", L"HANJA", L"Hanja"},
    {L"SYSTEM", L"system", L"action", L"MAIL_CLIENT", L"Mail"},
    {L"SYSTEM", L"system", L"action", L"MEDIA_CLIENT", L"Media"},
    {L"SYSTEM", L"system", L"action", L"HOME", L"BrowserHome"},
    {L"SYSTEM", L"system", L"action", L"APP_MENU", L"Apps"},
    {L"AUDIO", L"audio", L"action", L"MUTE_MICROPHONE", L"MicrophoneMute"},
    {L"MEDIA", L"media", L"usage", L"PLAY_PAUSE", L"PlayPause"},
    {L"MEDIA", L"media", L"usage", L"STOP", L"MediaStop"},
    {L"MEDIA", L"media", L"usage", L"PREVIOUS_TRACK", L"PreviousTrack"},
    {L"MEDIA", L"media", L"usage", L"NEXT_TRACK", L"NextTrack"},
    {L"MEDIA", L"media", L"usage", L"VOLUME_UP", L"VolumeUp"},
    {L"MEDIA", L"media", L"usage", L"VOLUME_DOWN", L"VolumeDown"},
    {L"MEDIA", L"media", L"usage", L"MUTE", L"Mute"},
    {L"QUICK_LAUNCH", L"quickLaunch", L"action", L"CALCULATOR", L"Open(calc.exe)"},
    {L"MOUSE", L"mouse", L"action", L"WIN_BACK", L"MouseBack"},
    {L"MOUSE", L"mouse", L"action", L"WIN_FORWARD", L"MouseForward"},
    {L"DO_NOTHING", L"doNothing", L"action", L"NONE", L"None"},
};
}
