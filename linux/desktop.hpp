#pragma once
#include "mapping.hpp"
#include "wayland_desktop.hpp"
#include <memory>

namespace loo_linux {
// Selects native GNOME/KDE provider on Wayland; preserves the X11 implementation.
// The emitter is required for native Wayland injection, never an XTEST fallback.
// Call on one event-loop thread. poll_hotkeys also services clipboard requests.
class Desktop {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit Desktop(KeyEmitter emitter={});
    ~Desktop();
    Desktop(const Desktop&)=delete;
    Desktop& operator=(const Desktop&)=delete;
    bool available() const;
    const std::string& reason() const;
    std::string status_json() const;
    std::vector<std::string> foreground();
    void validate(const Config& config) const;
    // A conflicting grab throws and leaves the previous registration intact.
    void configure_hotkeys(const Config& config);
    // Suspend passive grabs while emitting actions, so synthetic shortcuts reach
    // the application instead of recursively invoking the same global trigger.
    void suspend_hotkeys(bool suspended);
    // Triggers are returned after their main key and physical modifiers release.
    std::vector<std::vector<Action>> poll_hotkeys();
    // Asynchronous UTF-8 clipboard paste. Changes clipboard contents; target must
    // implement Ctrl+V. busy ends after source data read (not target UI insertion).
    void type_text(std::string_view utf8);
    bool busy() const;
    // XTEST on X11; supplied native emitter on Wayland.
    void inject_chord(const std::vector<unsigned>& keys);
};
}
