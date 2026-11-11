#pragma once
#include "mapping.hpp"
#include <functional>
#include <memory>

namespace loo_linux {
using KeyEmitter=std::function<void(const std::vector<unsigned>&)>;
// Native compositor provider protocol, never an Xwayland compatibility path.
// One private session-bus connection owns all this instance's registrations.
class WaylandDesktop {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit WaylandDesktop(KeyEmitter emitter);
    ~WaylandDesktop();
    WaylandDesktop(const WaylandDesktop&)=delete;
    WaylandDesktop& operator=(const WaylandDesktop&)=delete;
    bool available() const;
    const std::string& reason() const;
    std::string status_json() const;
    std::vector<std::string> foreground();
    void validate(const Config&) const;
    void configure_hotkeys(const Config&);
    void suspend_hotkeys(bool);
    std::vector<std::vector<Action>> poll_hotkeys();
    void type_text(std::string_view);
    void cancel_actions();
    bool busy() const;
    void inject_chord(const std::vector<unsigned>&);
};
}
