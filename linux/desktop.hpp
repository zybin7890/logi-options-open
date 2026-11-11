#pragma once
#include "mapping.hpp"
#include <memory>

namespace loo_linux {
// X11 only. Xwayland cannot observe/control native Wayland applications, so an
// XDG_SESSION_TYPE=wayland or WAYLAND_DISPLAY session is explicitly unsupported.
// Call on one event-loop thread. poll_hotkeys also services clipboard requests.
class Desktop {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    Desktop();
    ~Desktop();
    Desktop(const Desktop&)=delete;
    Desktop& operator=(const Desktop&)=delete;
    bool available() const;
    const std::string& reason() const;
    std::vector<std::string> foreground();
    void validate(const Config& config) const;
    // A conflicting grab throws and leaves the previous registration intact.
    void configure_hotkeys(const Config& config);
    // Suspend passive grabs while emitting actions, so synthetic shortcuts reach
    // the application instead of recursively invoking the same global trigger.
    void suspend_hotkeys(bool suspended);
    // Triggers are returned after their main key and physical modifiers release.
    std::vector<std::vector<Action>> poll_hotkeys();
    // Asynchronous UTF-8 clipboard paste. Changes CLIPBOARD contents; the target
    // must implement Ctrl+V paste. Pump poll_hotkeys until busy()==false.
    void type_text(std::string_view utf8);
    bool busy() const;
    // XTEST input for preview/testing. Device daemon may keep using uinput keys.
    void inject_chord(const std::vector<unsigned>& keys);
};
}
