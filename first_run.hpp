#pragma once
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace loo {
inline constexpr char notice_text[] =
    "Logi Options Open is an independent third-party project. It is not affiliated "
    "with, endorsed by, or supported by Logitech.\n\n"
    "This is immature, experimental software. Features are incomplete and bugs may "
    "temporarily interrupt custom buttons, scrolling or gestures. Linux hardware "
    "support is experimental; software tests are not physical-device validation.\n\n"
    "Keep a way to stop the agent and restore standard input. No warranty is provided. "
    "Continuing acknowledges this notice; it does not create an account or send telemetry.";

inline std::filesystem::path notice_path() {
#ifdef _WIN32
    wchar_t directory[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,directory))) return {};
    return std::filesystem::path(directory)/L"logi-options-open"/L"disclaimer-v1.accepted";
#else
    const auto* xdg=std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::filesystem::path(xdg)/"logi-options-open"/"disclaimer-v1.accepted";
    const auto* home=std::getenv("HOME");
    if (!home || !*home) return {};
    return std::filesystem::path(home)/".config/logi-options-open/disclaimer-v1.accepted";
#endif
}
inline bool notice_required(const std::filesystem::path& path=notice_path()) {
    std::ifstream input(path);
    std::string value;
    return !std::getline(input,value) || value!="acknowledged-v1";
}
inline bool accept_notice(const std::filesystem::path& path=notice_path()) {
    if (path.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(),error);
    if (error) return false;
    std::ofstream output(path,std::ios::trunc);
    output<<"acknowledged-v1\n";
    output.close();
    return output.good() && !notice_required(path);
}
#ifdef _WIN32
inline bool show_first_run_notice() {
    if (!notice_required()) return true;
    constexpr auto text=L"Logi Options Open 是独立的第三方项目，与 Logitech（罗技）无隶属关系，未获其认可或支持。\n\n"
        L"当前版本尚不成熟，属于实验性软件，功能不完整，可能存在导致自定义按键、滚轮或手势暂时失效的问题。Linux 硬件支持仍属实验阶段。\n\n"
        L"请保留停止后台及恢复标准输入的方法。本软件不提供担保。\n\n"
        L"点击“确定”表示已了解并继续；点击“取消”退出，不接管鼠标。此提示不会登录账号或发送数据。\n\n"
        L"Independent third-party project, not affiliated with Logitech. Immature experimental software; no warranty. OK acknowledges and continues; Cancel exits.";
    if (MessageBoxW(nullptr,text,L"Logi Options Open — 首次使用须知 / First use",MB_OKCANCEL|MB_ICONINFORMATION|MB_DEFBUTTON2)!=IDOK) return false;
    if (accept_notice()) return true;
    MessageBoxW(nullptr,L"无法保存已阅读状态，本次不会接管鼠标。请检查用户配置目录的写入权限。",
                L"Logi Options Open",MB_OK|MB_ICONERROR);
    return false;
}
#endif
}
