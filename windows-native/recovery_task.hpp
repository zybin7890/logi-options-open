#pragma once

#include <windows.h>
#include <sddl.h>
#include <taskschd.h>
#include <wrl/client.h>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace recovery_task {
namespace detail {
using Microsoft::WRL::ComPtr;
using Bstr = std::unique_ptr<OLECHAR, decltype(&SysFreeString)>;
inline Bstr bstr(const wchar_t* text) { return {SysAllocString(text), SysFreeString}; }

struct Apartment {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Apartment() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

inline std::wstring current_sid() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD size{};
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    const bool ok = size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    LPWSTR raw{};
    if (!ok || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &raw))
        return {};
    std::wstring sid(raw);
    LocalFree(raw);
    return sid;
}

struct Scheduler {
    Apartment apartment;
    std::wstring sid = current_sid();
    Bstr name = bstr((L"LogiNative Recovery " + sid).c_str());
    ComPtr<ITaskFolder> folder;

    bool connect() {
        if (sid.empty() || !name || (FAILED(apartment.hr) && apartment.hr != RPC_E_CHANGED_MODE))
            return false;
        ComPtr<ITaskService> service;
        VARIANT empty{};
        const auto root = bstr(L"\\");
        return root && SUCCEEDED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                   IID_PPV_ARGS(service.GetAddressOf()))) &&
               SUCCEEDED(service->Connect(empty, empty, empty, empty)) &&
               SUCCEEDED(service->GetFolder(root.get(), &folder));
    }
};

inline constexpr auto subscription =
    L"<QueryList><Query Id='0' Path='System'><Select Path='System'>"
    L"*[System[Provider[@Name='User32'] and EventID=1073]]</Select></Query></QueryList>";
inline constexpr auto boundary = L"2020-01-01T00:00:00";
inline constexpr auto arguments = L"--recover-tray";

inline std::wstring xml_text(const std::wstring& value) {
    std::wstring result;
    for (auto c : value) {
        switch (c) {
        case L'&': result += L"&amp;"; break;
        case L'<': result += L"&lt;"; break;
        case L'>': result += L"&gt;"; break;
        default: result += c;
        }
    }
    return result;
}

inline std::wstring definition(const std::wstring& sid, const std::filesystem::path& exe) {
    return std::wstring(L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<Triggers><EventTrigger><Subscription>") + xml_text(subscription) +
        L"</Subscription></EventTrigger><CalendarTrigger>"
        L"<Repetition><Interval>PT1M</Interval><Duration>P1D</Duration></Repetition>"
        L"<StartBoundary>" + boundary + L"</StartBoundary>"
        L"<ScheduleByDay><DaysInterval>1</DaysInterval></ScheduleByDay></CalendarTrigger></Triggers>"
        L"<Principals><Principal id=\"Owner\"><UserId>" + sid +
        L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel>"
        L"</Principal></Principals><Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<RunOnlyIfIdle>false</RunOnlyIfIdle><ExecutionTimeLimit>PT0S</ExecutionTimeLimit></Settings>"
        L"<Actions Context=\"Owner\"><Exec><Command>" + xml_text(exe.native()) +
        L"</Command><Arguments>" + arguments + L"</Arguments><WorkingDirectory>" +
        xml_text(exe.parent_path().native()) + L"</WorkingDirectory></Exec></Actions></Task>";
}

template<class Object, class Getter>
inline std::wstring read_text(const ComPtr<Object>& object, Getter getter) {
    BSTR raw{};
    const HRESULT hr = (object.Get()->*getter)(&raw);
    const Bstr holder(raw, SysFreeString);
    return SUCCEEDED(hr) && raw ? std::wstring(raw, SysStringLen(raw)) : std::wstring();
}

template<class Object, class Value>
inline bool equals(const ComPtr<Object>& object, HRESULT (STDMETHODCALLTYPE Object::*getter)(Value*),
                   std::type_identity_t<Value> expected) {
    Value value{};
    return SUCCEEDED((object.Get()->*getter)(&value)) && value == expected;
}

inline bool is_user(const std::wstring& user, const std::wstring& expected) {
    if (user == expected) return true;
    if (user.empty()) return false;
    DWORD sid_size{}, domain_size{};
    SID_NAME_USE type{};
    LookupAccountNameW(nullptr, user.c_str(), nullptr, &sid_size, nullptr, &domain_size, &type);
    if (!sid_size) return false;
    std::vector<BYTE> sid(sid_size);
    std::wstring domain(domain_size, L'\0');
    if (!LookupAccountNameW(nullptr, user.c_str(), sid.data(), &sid_size, domain.data(), &domain_size, &type))
        return false;
    LPWSTR text{};
    if (!ConvertSidToStringSidW(sid.data(), &text)) return false;
    const bool match = expected == text;
    LocalFree(text);
    return match;
}

inline bool has_triggers(const ComPtr<ITaskDefinition>& task) {
    ComPtr<ITriggerCollection> triggers;
    if (FAILED(task->get_Triggers(&triggers)) || !equals(triggers, &ITriggerCollection::get_Count, 2))
        return false;
    bool found_event{}, found_daily{};
    for (LONG i = 1; i <= 2; ++i) {
        ComPtr<ITrigger> trigger;
        if (FAILED(triggers->get_Item(i, &trigger)) || !equals(trigger, &ITrigger::get_Enabled, VARIANT_TRUE) ||
            !read_text(trigger, &ITrigger::get_EndBoundary).empty()) return false;
        ComPtr<IEventTrigger> event;
        ComPtr<IDailyTrigger> daily;
        if (SUCCEEDED(trigger.As(&event))) {
            found_event = read_text(event, &IEventTrigger::get_Subscription) == subscription;
        } else if (SUCCEEDED(trigger.As(&daily))) {
            ComPtr<IRepetitionPattern> repeat;
            found_daily = equals(daily, &IDailyTrigger::get_DaysInterval, 1) &&
                read_text(trigger, &ITrigger::get_StartBoundary) == boundary &&
                SUCCEEDED(trigger->get_Repetition(&repeat)) &&
                read_text(repeat, &IRepetitionPattern::get_Interval) == L"PT1M" &&
                read_text(repeat, &IRepetitionPattern::get_Duration) == L"P1D" &&
                equals(repeat, &IRepetitionPattern::get_StopAtDurationEnd, VARIANT_FALSE);
        }
    }
    return found_event && found_daily;
}

inline bool matches(const ComPtr<IRegisteredTask>& registered, const std::wstring& sid,
                    const std::filesystem::path& exe) {
    ComPtr<ITaskDefinition> task;
    ComPtr<IPrincipal> principal;
    ComPtr<ITaskSettings> settings;
    ComPtr<IActionCollection> actions;
    ComPtr<IAction> action;
    ComPtr<IExecAction> exec;
    return equals(registered, &IRegisteredTask::get_Enabled, VARIANT_TRUE) &&
        SUCCEEDED(registered->get_Definition(&task)) &&
        SUCCEEDED(task->get_Principal(&principal)) &&
        is_user(read_text(principal, &IPrincipal::get_UserId), sid) &&
        equals(principal, &IPrincipal::get_RunLevel, TASK_RUNLEVEL_LUA) &&
        equals(principal, &IPrincipal::get_LogonType, TASK_LOGON_INTERACTIVE_TOKEN) &&
        SUCCEEDED(task->get_Settings(&settings)) &&
        equals(settings, &ITaskSettings::get_MultipleInstances, TASK_INSTANCES_IGNORE_NEW) &&
        equals(settings, &ITaskSettings::get_DisallowStartIfOnBatteries, VARIANT_FALSE) &&
        equals(settings, &ITaskSettings::get_StopIfGoingOnBatteries, VARIANT_FALSE) &&
        equals(settings, &ITaskSettings::get_RunOnlyIfIdle, VARIANT_FALSE) &&
        read_text(settings, &ITaskSettings::get_ExecutionTimeLimit) == L"PT0S" &&
        has_triggers(task) &&
        SUCCEEDED(task->get_Actions(&actions)) && equals(actions, &IActionCollection::get_Count, 1) &&
        SUCCEEDED(actions->get_Item(1, &action)) && SUCCEEDED(action.As(&exec)) &&
        read_text(exec, &IExecAction::get_Path) == exe.native() &&
        read_text(exec, &IExecAction::get_Arguments) == arguments &&
        read_text(exec, &IExecAction::get_WorkingDirectory) == exe.parent_path().native();
}
} // namespace detail

inline bool ensure(const std::filesystem::path& exe) {
    if (!exe.is_absolute() || !exe.has_filename()) return false;
    const auto path = exe.lexically_normal();
    detail::Scheduler scheduler;
    if (!scheduler.connect()) return false;
    detail::ComPtr<IRegisteredTask> existing;
    if (SUCCEEDED(scheduler.folder->GetTask(scheduler.name.get(), &existing)) &&
        detail::matches(existing, scheduler.sid, path)) return true;
    const auto xml = detail::bstr(detail::definition(scheduler.sid, path).c_str());
    const auto user = detail::bstr(scheduler.sid.c_str());
    if (!xml || !user) return false;
    VARIANT identity{}, empty{};
    identity.vt = VT_BSTR;
    identity.bstrVal = user.get();
    detail::ComPtr<IRegisteredTask> result;
    return SUCCEEDED(scheduler.folder->RegisterTask(scheduler.name.get(), xml.get(), TASK_CREATE_OR_UPDATE,
        identity, empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, &result));
}

inline bool remove() {
    detail::Scheduler scheduler;
    if (!scheduler.connect()) return false;
    const auto hr = scheduler.folder->DeleteTask(scheduler.name.get(), 0);
    return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}
} // namespace recovery_task
