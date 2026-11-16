#include "ring.hpp"
#include "ring_visual.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <memory>

namespace {

constexpr int width = 536;
constexpr int height = 310;
constexpr int action_id = 100;
constexpr int caption_id = 120;
constexpr wchar_t class_name[] = L"LogiNativeActionRing";
constexpr COLORREF background = RGB(25, 25, 25);
constexpr COLORREF tile = RGB(6, 6, 6);
constexpr COLORREF white = RGB(245, 245, 245);
constexpr COLORREF accent = RGB(0, 234, 208);
constexpr int diameter = 42;
constexpr int tag_width = 118;
constexpr int tag_height = 30;
struct Slot {
    POINT center;
    POINT tag;
};
constexpr Slot slots[]{
    {{268, 81}, {209, 20}}, {{320, 103}, {349, 64}},
    {{342, 155}, {371, 140}}, {{320, 207}, {349, 216}},
    {{268, 229}, {209, 260}}, {{216, 207}, {69, 216}},
    {{194, 155}, {47, 140}}, {{216, 103}, {69, 64}}
};

int px(int logical, float scale = 1.0f) {
    return MulDiv(logical, static_cast<int>(GetDpiForSystem() * scale), 96);
}

struct Ring {
    std::span<const std::wstring> labels;
    bool submenu;
    std::span<const RingIcon> icons;
    float scale{1.0f};
    std::array<std::unique_ptr<Gdiplus::Image>,8> images;
    mutable RingFonts fonts{};
    HFONT font{};
    HFONT glyph_font{};
    int selection = -1;
    bool finished = false;
};

std::wstring_view glyph(std::wstring_view label, unsigned index) {
    constexpr std::pair<std::wstring_view, std::wstring_view> known[]{
        {L"Play / Pause", L"▶"}, {L"Media", L"♪"}, {L"New Note", L"✎"},
        {L"Mute", L"⊘"}, {L"Task View", L"▣"}, {L"Show desktop", L"□"},
        {L"File Explorer", L"▤"}, {L"Screenshot", L"⊡"},
        {L"Previous track", L"◀"}, {L"Next track", L"▶"}
    };
    for (const auto& [name, icon] : known)
        if (name == label) return icon;
    return std::wstring_view{L"12345678"}.substr(index, 1);
}

HWND make_button(HWND window, const wchar_t* title, int id, int x, int y, int w, int h, float scale,
                 DWORD extra = 0) {
    return CreateWindowW(L"BUTTON", title, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | extra,
                         px(x, scale), px(y, scale), px(w, scale), px(h, scale), window,
                         reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                         GetModuleHandleW(nullptr), nullptr);
}

void draw_item(const Ring& ring, const DRAWITEMSTRUCT& item) {
    const auto id = item.CtlID;
    const bool tag = id >= caption_id;
    const bool center = id == action_id - 1;
    const auto dc = item.hDC;
    auto bounds = item.rcItem;
    const auto brush = GetStockObject(DC_BRUSH);
    const auto old_brush = SelectObject(dc, brush);
    const auto old_pen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, background);
    FillRect(dc, &bounds, reinterpret_cast<HBRUSH>(brush));
    const bool active = (item.itemState & (ODS_SELECTED | ODS_FOCUS)) != 0;
    const auto fill = active ? accent : center ? RGB(43, 43, 43) : tile;
    SetDCBrushColor(dc, fill);
    SetDCPenColor(dc, tag && !active ? RGB(49, 49, 49) : fill);
    if (tag) RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, px(5, ring.scale), px(5, ring.scale));
    else Ellipse(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);

    if(!tag&&!center&&id-action_id<ring.icons.size()) {
        Gdiplus::Graphics graphics(dc);
        const float size=static_cast<float>(std::min(bounds.right,bounds.bottom)-2);
        draw_ring_visual(graphics,ring.icons[id-action_id],ring.images[id-action_id].get(),
            (bounds.right-size)/2,(bounds.bottom-size)/2,size,&ring.fonts);
        return;
    }
    const auto label = center ? std::wstring_view{ring.submenu ? L"‹" : L"×"}
        : tag ? std::wstring_view{ring.labels[id - caption_id]}
              : glyph(ring.labels[id - action_id], id - action_id);
    const auto old_font = SelectObject(dc, tag ? ring.font : ring.glyph_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, active ? tile : white);
    if (tag) InflateRect(&bounds, -px(7, ring.scale), 0);
    DrawTextW(dc, label.data(), static_cast<int>(label.size()), &bounds,
              DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old_font);
}

LRESULT CALLBACK ring_window(HWND window, UINT message, WPARAM parameter, LPARAM data) {
    auto* ring = reinterpret_cast<Ring*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_CREATE) {
        ring = static_cast<Ring*>(reinterpret_cast<CREATESTRUCTW*>(data)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ring));
        ring->font = CreateFontW(-px(13, ring->scale), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        ring->glyph_font = CreateFontW(-px(18, ring->scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Symbol");
        make_button(window, ring->submenu ? L"返回" : L"关闭", action_id - 1,
                    254, 141, 28, 28, ring->scale, WS_TABSTOP);
        for (unsigned i = 0; i < ring->labels.size(); ++i) {
            if (ring->labels[i].empty()) continue;
            const auto& slot = slots[i];
            make_button(window, ring->labels[i].c_str(), action_id + i,
                        slot.center.x - diameter / 2, slot.center.y - diameter / 2,
                        diameter, diameter, ring->scale, WS_TABSTOP);
            make_button(window, ring->labels[i].c_str(), caption_id + i,
                        slot.tag.x, slot.tag.y, tag_width, tag_height, ring->scale);
        }
        return 0;
    }
    if (ring && message == WM_DRAWITEM) {
        draw_item(*ring, *reinterpret_cast<const DRAWITEMSTRUCT*>(data));
        return TRUE;
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        SetDCBrushColor(dc, background);
        FillRect(dc, &paint.rcPaint, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        EndPaint(window, &paint);
        return 0;
    }
    if (ring && message == WM_COMMAND && HIWORD(parameter) == BN_CLICKED) {
        const int id = LOWORD(parameter);
        const int index = id - (id >= caption_id ? caption_id : action_id);
        if (id == action_id - 1) {
            if (ring->submenu) ring->selection = ring_back;
            ring->finished = true;
        } else if (index >= 0 && static_cast<unsigned>(index) < ring->labels.size() &&
                   !ring->labels[index].empty()) {
            ring->selection = index;
            ring->finished = true;
        }
        return 0;
    }
    if (ring && message == WM_CLOSE) {
        ring->finished = true;
        return 0;
    }
    if(ring&&message==WM_ACTIVATE&&LOWORD(parameter)==WA_INACTIVE)ring->finished=true;
    if (ring && message == WM_NCDESTROY) {
        if (ring->font) DeleteObject(ring->font);
        if (ring->glyph_font) DeleteObject(ring->glyph_font);
    }
    return DefWindowProcW(window, message, parameter, data);
}

} // namespace

int choose_ring_action(std::span<const std::wstring> labels, HANDLE stop, bool submenu,std::span<const RingIcon> icons, float scale) {
    if (labels.empty() || labels.size() > std::size(slots)) return -1;
    scale = std::clamp(scale, 0.7f, 1.5f);
    ULONG_PTR graphics_token{};
    Gdiplus::GdiplusStartupInput graphics_input;
    if(Gdiplus::GdiplusStartup(&graphics_token,&graphics_input,nullptr)!=Gdiplus::Ok)return -1;
    struct GraphicsLifetime {ULONG_PTR token;~GraphicsLifetime(){Gdiplus::GdiplusShutdown(token);}} graphics_lifetime{graphics_token};
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW cls{};
    cls.lpfnWndProc = ring_window;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = class_name;
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return -1;

    const auto previous = GetForegroundWindow();
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor)) return -1;
    constexpr DWORD style = WS_POPUP | WS_CLIPCHILDREN;
    const DWORD extended = WS_EX_TOPMOST | (stop ? WS_EX_TOOLWINDOW : WS_EX_APPWINDOW);
    const auto outer_width = px(width, scale);
    const auto outer_height = px(height, scale);
    const auto& area = monitor.rcWork;
    const auto left = std::clamp(cursor.x - outer_width / 2, area.left,
                                 std::max(area.left, area.right - outer_width));
    const auto top = std::clamp(cursor.y - outer_height / 2, area.top,
                                std::max(area.top, area.bottom - outer_height));

    Ring ring{labels, submenu, icons, scale, {}};
    for(std::size_t i=0;i<std::min(icons.size(),ring.images.size());++i)
        if(!icons[i].image.empty())ring.images[i]=std::make_unique<Gdiplus::Image>(icons[i].image.c_str());
    const auto window = CreateWindowExW(extended, class_name,
                                        L"LogiNative Actions Ring", style, left, top,
                                        outer_width, outer_height, nullptr, nullptr, instance, &ring);
    if (!window) return -1;
    if (const auto region = CreateRoundRectRgn(0, 0, outer_width + 1, outer_height + 1, px(14, scale), px(14, scale)))
        if (!SetWindowRgn(window, region, FALSE)) DeleteObject(region);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    const DWORD count = stop ? 1 : 0;
    while (!ring.finished) {
        const auto wait = MsgWaitForMultipleObjectsEx(count, stop ? &stop : nullptr, INFINITE,
                                                       QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait != WAIT_OBJECT_0 + count) break;
        MSG msg{};
        while (!ring.finished && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT || (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)) {
                ring.finished = true;
            } else if (msg.message == WM_KEYDOWN && msg.wParam >= '1' &&
                       msg.wParam < '1' + labels.size() && !labels[msg.wParam-'1'].empty()) {
                ring.selection = static_cast<int>(msg.wParam - '1');
                ring.finished = true;
            } else if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN &&
                       GetParent(GetFocus()) == window) {
                SendMessageW(GetFocus(), BM_CLICK, 0, 0);
            } else if (!IsDialogMessageW(window, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    DestroyWindow(window);
    if (ring.selection >= 0 && previous && IsWindow(previous)) SetForegroundWindow(previous);
    return ring.selection;
}
