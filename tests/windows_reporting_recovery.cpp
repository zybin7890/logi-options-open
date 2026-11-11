// Explicit local hardware fault-injection test; never installed with the app.
#include "mouse.hpp"
#include "remap.hpp"
#include <cstdio>
#include <string_view>
int wmain(int argc,wchar_t** argv) {
    if(argc!=2 || std::wstring_view(argv[1])!=L"--reset-reporting-while-agent-runs" || !agent_running())return 2;
    auto mouse=logi::connect();
    if(!mouse || mouse->device.name!="MX Master 3S")return 3;
    // Simulate device/driver resetting custom diversion while the receiver and
    // daemon process remain online. The application's own watchdog must repair.
    for(auto id:{0x53u,0xC3u})if(!mouse->request(mouse->buttons,3,{logi::high(id),logi::low(id),0x22}))return 4;
    if(!mouse->request(mouse->thumb,2,{0,0}))return 5;
    auto main=mouse->request(mouse->wheel,1);if(!main)return 6;
    if(!mouse->request(mouse->wheel,2,{static_cast<logi::Byte>(main.packet.data()[0]|1)}))return 7;
    std::puts("Injected actual hardware reporting reset: side button, gesture, thumb wheel, main wheel.");
    std::fflush(stdout);
    // Simulate resumed standard pointer activity, without a click or key action.
    INPUT activity[2]{};activity[0].type=activity[1].type=INPUT_MOUSE;
    activity[0].mi.dwFlags=activity[1].mi.dwFlags=MOUSEEVENTF_MOVE;
    activity[0].mi.dx=1;activity[1].mi.dx=-1;
    if(SendInput(2,activity,sizeof(INPUT))!=2)return 9;
    const auto deadline=GetTickCount64()+7000;
    do {
        Sleep(200);
        const auto side=mouse->request(mouse->buttons,2,{0,0x53});
        const auto gesture=mouse->request(mouse->buttons,2,{0,0xC3});
        const auto thumb=mouse->request(mouse->thumb,1);
        main=mouse->request(mouse->wheel,1);
        if(side && gesture && thumb && main && (side.packet.data()[2]&0x11)==1 &&
            (gesture.packet.data()[2]&0x11)==0x11 && thumb.packet.data()[0]==1 && !(main.packet.data()[0]&1)) {
            std::puts("PASS: all four actual device reporting states recovered without restarting the daemon.");return 0;
        }
    }while(GetTickCount64()<deadline);
    std::fputs("FAIL: automatic device reporting recovery did not complete.\n",stderr);return 8;
}
