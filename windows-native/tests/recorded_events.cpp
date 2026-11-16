#include "environment.hpp"

int main(int argc, char** argv) { try {
    CHECK(argc == 3);
    const std::string scenario = argv[1];
    fixture_path = argv[2]; restore_ok = true; mode = "recorded-events";
    std::ofstream(fixture_path) << "[global]\n0052=KeyDown(LeftCtrl);KeyDown(A);Wait(200);KeyUp(A);KeyUp(LeftCtrl);B\n";
    Step press; press.down = true;
    Step due; due.at = 500;
    steps = {press, {}, due};
    if (scenario == "stop") { Step stop; stop.stop = true; steps = {press, stop}; }
    else if (scenario == "disconnect") { Step disconnect; disconnect.disconnect = true; steps = {press, disconnect, due}; }
    else if (scenario == "loss") { steps[1].loss = true; }
    else if (scenario == "reload") {
        before_step = [](unsigned index) {
            if (index == 2) { std::ofstream(fixture_path) << "[global]\n0052=None\n"; reload_signal = true; }
        };
    }
    else if (scenario == "focus") { steps[1].app = 2; }
    else if (scenario != "complete") throw std::runtime_error("unknown recorded event case");
    CHECK(run_agent(fixture_path.c_str(), false) == 0);
    CHECK(connects == (scenario == "disconnect" ? 2u : 1u));
    CHECK(captured.size() == (scenario == "complete" ? 6u : 4u));
    CHECK(captured[0].ki.wVk == VK_LCONTROL && !(captured[0].ki.dwFlags & KEYEVENTF_KEYUP));
    CHECK(captured[1].ki.wVk == 'A' && !(captured[1].ki.dwFlags & KEYEVENTF_KEYUP));
    CHECK(captured[2].ki.wVk == 'A' && (captured[2].ki.dwFlags & KEYEVENTF_KEYUP));
    CHECK(captured[3].ki.wVk == VK_LCONTROL && (captured[3].ki.dwFlags & KEYEVENTF_KEYUP));
    CHECK(capture_times[2] == (scenario == "complete" ? 500u : 100u));
    if (scenario == "complete") CHECK(captured[4].ki.wVk == 'B' && captured[5].ki.wVk == 'B');
    std::cout << "PASS actual run_agent/events " << scenario << ": recorded holds released, continuation bounded by lifecycle\n";
    return 0;
} catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; } }
