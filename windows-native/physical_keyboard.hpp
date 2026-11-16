#pragma once

#include <windows.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

// GetAsyncKeyState combines physical and injected input. Observe only real
// keyboard events so a recorded hold never releases a key the user took over.
// The observer starts only when a recorded KeyDown is first executed.
class PhysicalKeyboard {
    std::array<std::atomic_bool, 256> down_{};
    std::thread thread_;
    DWORD thread_id_{};
    bool ready_{}, installed_{};
    std::mutex mutex_;
    std::condition_variable started_;
    inline static thread_local PhysicalKeyboard* current_{};

    static LRESULT CALLBACK observe(int code, WPARAM message, LPARAM data) {
        if (code == HC_ACTION && current_) {
            const auto& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
            if (!(event.flags & LLKHF_INJECTED) && event.vkCode < 256) {
                auto key = event.vkCode;
                if (key == VK_CONTROL) key = event.flags & LLKHF_EXTENDED ? VK_RCONTROL : VK_LCONTROL;
                if (key == VK_MENU) key = event.flags & LLKHF_EXTENDED ? VK_RMENU : VK_LMENU;
                if (key == VK_SHIFT) key = MapVirtualKeyW(event.scanCode, MAPVK_VSC_TO_VK_EX);
                if (key < 256) current_->down_[key].store(!(event.flags & LLKHF_UP));
            }
        }
        return CallNextHookEx(nullptr, code, message, data);
    }

public:
    PhysicalKeyboard() = default;
    PhysicalKeyboard(const PhysicalKeyboard&) = delete;
    ~PhysicalKeyboard() {
        if (thread_.joinable()) {
            PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
            thread_.join();
        }
    }
    bool start() {
        if (ready_) return installed_;
        thread_ = std::thread([this] {
            MSG message{};
            PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
            thread_id_ = GetCurrentThreadId();
            current_ = this;
            for (unsigned key = 0; key < down_.size(); ++key)
                down_[key].store((GetAsyncKeyState(key) & 0x8000) != 0);
            const auto hook = SetWindowsHookExW(WH_KEYBOARD_LL, observe, GetModuleHandleW(nullptr), 0);
            {
                std::lock_guard lock{mutex_};
                installed_ = hook != nullptr;
                ready_ = true;
            }
            started_.notify_one();
            if (hook) {
                while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                UnhookWindowsHookEx(hook);
            }
            current_ = nullptr;
        });
        std::unique_lock lock{mutex_};
        started_.wait(lock, [this] { return ready_; });
        return installed_;
    }
    bool down(WORD key) const { return down_[key].load(); }
};
