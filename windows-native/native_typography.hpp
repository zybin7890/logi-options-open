#pragma once

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <dwrite_3.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace native_typography {

// Original font files are optional local resources, not part of the public package.
// Call initialize after GdiplusStartup, and shutdown after all UI fonts/windows have
// been destroyed but before GdiplusShutdown. These functions run on the UI thread.
struct Face {
    const wchar_t* file;
    const wchar_t* family;
    int style;
};
inline constexpr std::array<Face, 11> faces{{
    {L"BrownPro-Regular.ttf", L"BrownPro", Gdiplus::FontStyleRegular},
    {L"BrownPro-Bold.ttf", L"BrownPro", Gdiplus::FontStyleBold},
    {L"BrownPro-Light.ttf", L"BrownPro Light", Gdiplus::FontStyleRegular},
    {L"MyriadPro-Regular.ttf", L"Myriad Pro", Gdiplus::FontStyleRegular},
    {L"MyriadPro-Bold.ttf", L"Myriad Pro", Gdiplus::FontStyleBold},
    {L"MyriadPro-Light.ttf", L"Myriad Pro Light", Gdiplus::FontStyleRegular},
    {L"BrownLogitechPan-Regular.otf", L"Brown Logitech Pan", Gdiplus::FontStyleRegular},
    {L"BrownLogitechPan-Bold.otf", L"Brown Logitech Pan", Gdiplus::FontStyleBold},
    {L"BrownLogitechPan-Light.otf", L"Brown Logitech Pan Light", Gdiplus::FontStyleRegular},
    {L"BrownLogitechPan-Medium.otf", L"Brown Logitech Pan Medium", Gdiplus::FontStyleRegular},
    {L"BrownLogitechPan-Thin.otf", L"Brown Logitech Pan Thin", Gdiplus::FontStyleRegular},
}};

template<class T> class Com {
    T* value{};
public:
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& other) noexcept : value(other.value) { other.value = nullptr; }
    Com& operator=(Com&& other) noexcept {
        if (this != &other) {
            if (value) value->Release();
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
    ~Com() { if (value) value->Release(); }
    T* get() const { return value; }
    T* operator->() const { return value; }
    explicit operator bool() const { return value != nullptr; }
    T** put() { if (value) value->Release(); value = nullptr; return &value; }
};

struct LayoutCacheEntry {
    float size{};
    LONG weight{};
    BYTE italic{}, underline{}, strikeout{};
    bool ring{};
    UINT flags{};
    int width{}, height{};
    std::wstring text;
    Com<IDWriteTextLayout> layout;
};

struct WriteFonts {
    HMODULE dwrite_dll{};
    Com<IDWriteFactory3> factory;
    Com<IDWriteFontCollection1> originals;
    Com<IDWriteFontCollection> system;
    Com<IDWriteGdiInterop> interop;
    Com<IDWriteRenderingParams> rendering;
    Com<IDWriteBitmapRenderTarget> target;
    UINT target_width{}, target_height{};
    std::array<LayoutCacheEntry, 32> layout_cache{};
    std::size_t layout_cache_next{};

    explicit WriteFonts(const std::filesystem::path& directory) {
        dwrite_dll = LoadLibraryW(L"dwrite.dll");
        if (!dwrite_dll) return;
        using DWriteCreateFactoryFn = HRESULT (WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
        DWriteCreateFactoryFn create_dwrite{};
        const FARPROC proc = GetProcAddress(dwrite_dll, "DWriteCreateFactory");
        if (!proc) return;
        std::memcpy(&create_dwrite, &proc, sizeof(create_dwrite));
        if (FAILED(create_dwrite(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory3),
                                 reinterpret_cast<IUnknown**>(factory.put())))) return;
        auto* base = static_cast<IDWriteFactory*>(factory.get());
        base->GetSystemFontCollection(system.put(), FALSE);
        base->GetGdiInterop(interop.put());
        base->CreateRenderingParams(rendering.put());
        Com<IDWriteFontSetBuilder> builder;
        if (FAILED(factory->CreateFontSetBuilder(builder.put()))) return;
        for (const auto& face : faces) {
            const auto path = directory / face.file;
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error)) continue;
            Com<IDWriteFontFaceReference> reference;
            if (SUCCEEDED(factory->CreateFontFaceReference(path.c_str(), nullptr, 0, DWRITE_FONT_SIMULATIONS_NONE, reference.put())))
                builder->AddFontFaceReference(reference.get());
        }
        Com<IDWriteFontSet> set;
        if (SUCCEEDED(builder->CreateFontSet(set.put()))) factory->CreateFontCollectionFromFontSet(set.get(), originals.put());
    }
    ~WriteFonts() {
        for (auto& entry : layout_cache) entry.layout = {};
        target = {};
        rendering = {};
        interop = {};
        originals = {};
        system = {};
        factory = {};
        if (dwrite_dll) FreeLibrary(dwrite_dll);
    }
};

struct Fonts {
    std::array<std::vector<char>, faces.size()> bytes;
    std::array<HANDLE, faces.size()> gdi{};
    std::array<bool, faces.size()> loaded{};
    // GDI+ merges legacy family names when different weights share a collection.
    // Per-face collections preserve the original Light/Medium/Thin family names.
    std::array<std::unique_ptr<Gdiplus::PrivateFontCollection>, faces.size()> collections;
    std::array<std::unique_ptr<Gdiplus::FontFamily>, faces.size()> families;
    std::size_t count{};
    std::unique_ptr<WriteFonts> write;

    ~Fonts() {
        write.reset();
        for (auto& family : families) family.reset();
        for (auto& collection : collections) collection.reset();
        for (const auto handle : gdi) if (handle) RemoveFontMemResourceEx(handle);
    }
};
inline std::unique_ptr<Fonts> current;

inline std::filesystem::path defaultDirectory() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) return {};
    return std::filesystem::path(std::wstring(path.data(), length)).parent_path() / L"assets" / L"fonts";
}

inline std::size_t initialize(std::filesystem::path directory = {}) {
    if (current) return current->count;
    if (directory.empty()) directory = defaultDirectory();
    current = std::make_unique<Fonts>();
    current->write = std::make_unique<WriteFonts>(directory);
    if (directory.empty()) return 0;
    for (std::size_t i = 0; i < faces.size(); ++i) {
        std::ifstream file(directory / faces[i].file, std::ios::binary | std::ios::ate);
        if (!file) continue;
        const auto file_size = file.tellg();
        if (file_size <= 0) continue;
        auto& bytes = current->bytes[i];
        bytes.resize(static_cast<std::size_t>(file_size));
        file.seekg(0);
        if (!file.read(bytes.data(), file_size)) { bytes.clear(); continue; }
        auto& collection = current->collections[i];
        collection = std::make_unique<Gdiplus::PrivateFontCollection>();
        if (collection->AddMemoryFont(bytes.data(), static_cast<INT>(bytes.size())) != Gdiplus::Ok)
            continue;
        auto& family = current->families[i];
        family = std::make_unique<Gdiplus::FontFamily>();
        int found{};
        if (collection->GetFamilies(1, family.get(), &found) != Gdiplus::Ok || found != 1)
            continue;
        current->loaded[i] = true;
        ++current->count;
        DWORD count{};
        current->gdi[i] = AddFontMemResourceEx(bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, &count);
    }
    return current->count;
}

inline void shutdown() { current.reset(); }

inline std::size_t faceIndex(int weight, bool ring) {
    if (!ring) return weight >= FW_SEMIBOLD ? 1 : weight <= FW_LIGHT ? 2 : 0;
    if (weight >= FW_SEMIBOLD) return 7;
    if (weight <= FW_ULTRALIGHT) return 10;
    if (weight <= FW_LIGHT) return 8;
    return weight >= FW_MEDIUM ? 9 : 6;
}

inline const wchar_t* nativeFamily(int weight = FW_NORMAL, bool ring = false) {
    const auto index = faceIndex(weight, ring);
    return current && current->gdi[index] ? faces[index].family : L"Microsoft YaHei UI";
}

inline std::unique_ptr<Gdiplus::Font> drawFont(HDC dc, HFONT font, bool ring = false) {
    LOGFONTW logical{};
    logical.lfHeight = -15;
    logical.lfWeight = FW_NORMAL;
    if (font) GetObjectW(font, sizeof(logical), &logical);
    float size = static_cast<float>(logical.lfHeight < 0 ? -logical.lfHeight : logical.lfHeight);
    if (logical.lfHeight > 0 && dc && font) {
        const auto previous = SelectObject(dc, font);
        TEXTMETRICW metrics{};
        if (GetTextMetricsW(dc, &metrics)) size = static_cast<float>(metrics.tmHeight - metrics.tmInternalLeading);
        SelectObject(dc, previous);
    }
    if (size <= 0) size = 15.0f;
    int decorations = logical.lfItalic ? Gdiplus::FontStyleItalic : 0;
    if (logical.lfUnderline) decorations |= Gdiplus::FontStyleUnderline;
    if (logical.lfStrikeOut) decorations |= Gdiplus::FontStyleStrikeout;
    auto index = faceIndex(logical.lfWeight, ring);
    if (current && !current->loaded[index] && !ring) index += 3; // CSS's Myriad fallback.
    if (current && current->loaded[index]) {
        // Do not round-trip through Font(HDC,HFONT): memory-only fonts return
        // FontFamilyNotFound there. Keep the private collection alive until drawing ends.
        auto result = std::make_unique<Gdiplus::Font>(current->families[index].get(), size,
            faces[index].style | decorations, Gdiplus::UnitPixel);
        if (result->GetLastStatus() == Gdiplus::Ok) return result;
    }
    const int style = (logical.lfWeight >= FW_SEMIBOLD ? Gdiplus::FontStyleBold : 0) | decorations;
    return std::make_unique<Gdiplus::Font>(L"Microsoft YaHei UI", size, style, Gdiplus::UnitPixel);
}

inline bool cjkCodepoint(unsigned code) {
    return (code >= 0x2e80 && code <= 0x9fff) || (code >= 0xac00 && code <= 0xd7af) ||
           (code >= 0xf900 && code <= 0xfaff) || (code >= 0xff00 && code <= 0xffef) ||
           (code >= 0x20000 && code <= 0x323af);
}

// DirectWrite owns shaping, baselines, wrapping and trimming for the whole string.
// Font ranges change the CJK face without flattening mixed Latin/CJK text to one font.
inline Com<IDWriteTextLayout> textLayout(HDC dc, HFONT font, std::wstring_view value,
                                        float width, float height, UINT flags, bool ring = false) {
    Com<IDWriteTextLayout> layout;
    if (!current || !current->write || !current->write->factory) return layout;
    auto& engine = *current->write;
    LOGFONTW logical{};
    logical.lfHeight = -15; logical.lfWeight = FW_NORMAL;
    if (font) GetObjectW(font, sizeof(logical), &logical);
    float size = static_cast<float>(logical.lfHeight < 0 ? -logical.lfHeight : logical.lfHeight);
    if (logical.lfHeight > 0 && dc && font) {
        const auto previous = SelectObject(dc, font);
        TEXTMETRICW metrics{};
        if (GetTextMetricsW(dc, &metrics)) size = static_cast<float>(metrics.tmHeight - metrics.tmInternalLeading);
        SelectObject(dc, previous);
    }
    if (size <= 0) size = 15.0f;
    const int box_w = static_cast<int>(std::max(1.0f, width));
    const int box_h = static_cast<int>(std::max(1.0f, height));
    for (const auto& entry : engine.layout_cache) {
        if (entry.layout && entry.size == size && entry.weight == logical.lfWeight &&
            entry.italic == logical.lfItalic && entry.underline == logical.lfUnderline &&
            entry.strikeout == logical.lfStrikeOut && entry.ring == ring &&
            entry.flags == flags && entry.width == box_w && entry.height == box_h &&
            entry.text == value) {
            entry.layout->AddRef();
            *layout.put() = entry.layout.get();
            return layout;
        }
    }
    auto* collection = static_cast<IDWriteFontCollection*>(engine.originals.get());
    const wchar_t* family = nativeFamily(logical.lfWeight, ring);
    UINT32 index{}; BOOL exists{};
    if (!collection || FAILED(collection->FindFamilyName(family, &index, &exists)) || !exists) {
        collection = engine.system.get(); family = L"Microsoft YaHei UI";
    }
    Com<IDWriteTextFormat> format;
    if (FAILED(engine.factory->CreateTextFormat(family, collection,
        static_cast<DWRITE_FONT_WEIGHT>(std::clamp<LONG>(logical.lfWeight, 100, 900)),
        logical.lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", format.put()))) return layout;
    format->SetTextAlignment(flags & DT_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER :
        flags & DT_RIGHT ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(flags & DT_VCENTER ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    format->SetWordWrapping(flags & DT_SINGLELINE ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_WRAP);
    if (flags & DT_END_ELLIPSIS) {
        Com<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(engine.factory->CreateEllipsisTrimmingSign(format.get(), ellipsis.put()))) {
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            format->SetTrimming(&trimming, ellipsis.get());
        }
    }
    if (FAILED(engine.factory->CreateTextLayout(value.data(), static_cast<UINT32>(value.size()), format.get(),
                                               std::max(1.0f, width), std::max(1.0f, height), layout.put()))) return layout;
    for (size_t i = 0; i < value.size();) {
        const size_t start = i;
        while (i < value.size()) {
            const size_t char_start = i;
            unsigned code = value[i++];
            if (code >= 0xd800 && code <= 0xdbff && i < value.size() && value[i] >= 0xdc00 && value[i] <= 0xdfff)
                code = 0x10000 + ((code - 0xd800) << 10) + value[i++] - 0xdc00;
            if (!cjkCodepoint(code)) {
                if (char_start == start) break;
                i = char_start;
                break;
            }
        }
        if (i > start) {
            unsigned first = value[start];
            if (first >= 0xd800 && first <= 0xdbff && start + 1 < value.size() && value[start + 1] >= 0xdc00 && value[start + 1] <= 0xdfff)
                first = 0x10000 + ((first - 0xd800) << 10) + value[start + 1] - 0xdc00;
            if (cjkCodepoint(first)) {
                const DWRITE_TEXT_RANGE range{static_cast<UINT32>(start), static_cast<UINT32>(i - start)};
                layout->SetFontCollection(engine.system.get(), range);
                layout->SetFontFamilyName(L"Microsoft YaHei UI", range);
            }
        }
    }
    const DWRITE_TEXT_RANGE all{0, static_cast<UINT32>(value.size())};
    if (logical.lfUnderline) layout->SetUnderline(TRUE, all);
    if (logical.lfStrikeOut) layout->SetStrikethrough(TRUE, all);
    if (value.size() <= 128) {
        auto& slot = engine.layout_cache[engine.layout_cache_next++ % engine.layout_cache.size()];
        slot.size = size;
        slot.weight = logical.lfWeight;
        slot.italic = logical.lfItalic;
        slot.underline = logical.lfUnderline;
        slot.strikeout = logical.lfStrikeOut;
        slot.ring = ring;
        slot.flags = flags;
        slot.width = box_w;
        slot.height = box_h;
        slot.text.assign(value);
        layout->AddRef();
        *slot.layout.put() = layout.get();
    }
    return layout;
}

class HdcTextRenderer final : public IDWriteTextRenderer {
    IDWriteBitmapRenderTarget* target;
    IDWriteRenderingParams* rendering;
    COLORREF color;
    ULONG references{1};
    HRESULT decoration(float x, float y, float width, float height) {
        const RECT rect{static_cast<LONG>(std::floor(x)), static_cast<LONG>(std::floor(y)),
                        static_cast<LONG>(std::ceil(x + width)), static_cast<LONG>(std::ceil(y + height))};
        const auto brush = CreateSolidBrush(color);
        FillRect(target->GetMemoryDC(), &rect, brush); DeleteObject(brush);
        return S_OK;
    }
public:
    HdcTextRenderer(IDWriteBitmapRenderTarget* target, IDWriteRenderingParams* rendering, COLORREF color)
        : target(target), rendering(rendering), color(color) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWritePixelSnapping) || iid == __uuidof(IDWriteTextRenderer)) {
            *result = static_cast<IDWriteTextRenderer*>(this); AddRef(); return S_OK;
        }
        *result = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; } // Scoped to synchronous layout::Draw.
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override { *disabled = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* matrix) override { return target->GetCurrentTransform(matrix); }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels) override { *pixels = 1.0f; return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode,
        const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        return target->DrawGlyphRun(x, y, mode, run, rendering, color, nullptr);
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT x, FLOAT y, const DWRITE_UNDERLINE* line, IUnknown*) override {
        return decoration(x, y + line->offset, line->width, line->thickness);
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT x, FLOAT y, const DWRITE_STRIKETHROUGH* line, IUnknown*) override {
        return decoration(x, y + line->offset, line->width, line->thickness);
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void* context, FLOAT x, FLOAT y, IDWriteInlineObject* object,
        BOOL sideways, BOOL rtl, IUnknown* effect) override {
        return object->Draw(context, this, x, y, sideways, rtl, effect);
    }
};

inline Gdiplus::SizeF measureText(HDC dc, HFONT font, std::wstring_view value, bool ring = false,
                                 float width = 1000000.0f, UINT flags = DT_SINGLELINE) {
    const auto layout = textLayout(dc, font, value, width, 1000000.0f, flags, ring);
    DWRITE_TEXT_METRICS metrics{};
    if (!layout || FAILED(layout->GetMetrics(&metrics))) return {};
    return {metrics.widthIncludingTrailingWhitespace, metrics.height};
}

inline bool drawText(HDC dc, HFONT font, COLORREF color, std::wstring_view value, RECT rect,
                      UINT flags, bool ring = false) {
    if (value.empty()) return true;
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return true;
    const auto layout = textLayout(dc, font, value, static_cast<float>(width), static_cast<float>(height), flags, ring);
    if (!layout || !current->write->interop || !current->write->rendering) return false;
    auto& engine = *current->write;
    if (!engine.target) {
        if (FAILED(engine.interop->CreateBitmapRenderTarget(dc, width, height, engine.target.put()))) return false;
        engine.target_width = width; engine.target_height = height;
        engine.target->SetPixelsPerDip(1.0f);
    } else if (static_cast<UINT>(width) > engine.target_width || static_cast<UINT>(height) > engine.target_height) {
        engine.target_width = std::max(engine.target_width, static_cast<UINT>(width));
        engine.target_height = std::max(engine.target_height, static_cast<UINT>(height));
        if (FAILED(engine.target->Resize(engine.target_width, engine.target_height))) return false;
    }
    const auto memory = engine.target->GetMemoryDC();
    if (!BitBlt(memory, 0, 0, width, height, dc, rect.left, rect.top, SRCCOPY)) return false;
    HdcTextRenderer renderer(engine.target.get(), engine.rendering.get(), color);
    if (FAILED(layout->Draw(nullptr, &renderer, 0, 0))) return false;
    return BitBlt(dc, rect.left, rect.top, width, height, memory, 0, 0, SRCCOPY) != FALSE;
}

} // namespace native_typography
