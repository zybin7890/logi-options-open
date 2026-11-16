#include "screen_capture.hpp"
#include <windows.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>

namespace screen_capture {
namespace {
struct DpiScope {
    DPI_AWARENESS_CONTEXT previous{SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)};
    ~DpiScope(){if(previous)SetThreadDpiAwarenessContext(previous);}
};
struct Frame {
    HDC desktop{},memory{};HBITMAP bitmap{};HGDIOBJ previous{};
    BITMAPINFO info{};BYTE* pixels{};
    ~Frame(){if(previous&&previous!=HGDI_ERROR)SelectObject(memory,previous);if(bitmap)DeleteObject(bitmap);if(memory)DeleteDC(memory);if(desktop)ReleaseDC(nullptr,desktop);}
    bool read(Area area,const ActionContext& context) {
        DpiScope dpi;
        RECT rectangle{GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),0,0};
        rectangle.right=rectangle.left+GetSystemMetrics(SM_CXVIRTUALSCREEN);
        rectangle.bottom=rectangle.top+GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if(area==Area::window) {
            const auto window=context.window?context.window:GetForegroundWindow();if(!window||IsIconic(window))return false;
            RECT bounds{};
            if(FAILED(DwmGetWindowAttribute(window,DWMWA_EXTENDED_FRAME_BOUNDS,&bounds,sizeof(bounds))) && !GetWindowRect(window,&bounds))return false;
            if(!IntersectRect(&rectangle,&rectangle,&bounds))return false;
        }
        const auto width=rectangle.right-rectangle.left,height=rectangle.bottom-rectangle.top;
        if(width<=0||height<=0||std::uint64_t(width)*height*4>std::numeric_limits<DWORD>::max())return false;
        info.bmiHeader={sizeof(BITMAPINFOHEADER),width,-height,1,32,BI_RGB,static_cast<DWORD>(width*std::uint64_t(height)*4),0,0,0,0};
        desktop=GetDC(nullptr);if(!desktop)return false;
        memory=CreateCompatibleDC(desktop);if(!memory)return false;
        bitmap=CreateDIBSection(desktop,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels),nullptr,0);
        if(!bitmap||!pixels)return false;
        previous=SelectObject(memory,bitmap);if(!previous||previous==HGDI_ERROR)return false;
        return context.active() && BitBlt(memory,0,0,width,height,desktop,rectangle.left,rectangle.top,SRCCOPY|CAPTUREBLT)!=FALSE;
    }
};
bool clipboard(const Frame& frame,const ActionContext& context) {
    // A real owner window is required: EmptyClipboard with a null owner causes
    // SetClipboardData to fail. This message-only window is never displayed.
    const auto owner=CreateWindowExW(0,L"STATIC",L"",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!owner)return false;
    if(!OpenClipboard(owner)){DestroyWindow(owner);return false;}
    const auto bytes=sizeof(BITMAPINFOHEADER)+frame.info.bmiHeader.biSizeImage;
    const auto memory=GlobalAlloc(GMEM_MOVEABLE,bytes);
    bool accepted=false;
    if(memory) {
        if(auto data=static_cast<BYTE*>(GlobalLock(memory))) {
            std::memcpy(data,&frame.info.bmiHeader,sizeof(BITMAPINFOHEADER));
            std::memcpy(data+sizeof(BITMAPINFOHEADER),frame.pixels,frame.info.bmiHeader.biSizeImage);
            GlobalUnlock(memory);
            accepted=context.active()&&EmptyClipboard()&&SetClipboardData(CF_DIB,memory)!=nullptr;
        }
        if(!accepted)GlobalFree(memory);
    }
    CloseClipboard();DestroyWindow(owner);return accepted;
}
struct GdiPlus {
    ULONG_PTR token{};
    GdiPlus(){Gdiplus::GdiplusStartupInput input; if(Gdiplus::GdiplusStartup(&token,&input,nullptr)!=Gdiplus::Ok)token=0;}
    ~GdiPlus(){if(token)Gdiplus::GdiplusShutdown(token);}
};
bool file(const Frame& frame,const ActionContext& context) {
    GdiPlus scope;if(!scope.token)return false;
    Gdiplus::Bitmap bitmap(frame.info.bmiHeader.biWidth,-frame.info.bmiHeader.biHeight,
        frame.info.bmiHeader.biWidth*4,PixelFormat32bppRGB,frame.pixels);
    if(bitmap.GetLastStatus()!=Gdiplus::Ok)return false;
    constexpr CLSID png{0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
    IStream* stream{};if(FAILED(CreateStreamOnHGlobal(nullptr,TRUE,&stream)))return false;
    if(bitmap.Save(stream,&png,nullptr)!=Gdiplus::Ok){stream->Release();return false;}
    HGLOBAL encoded{};STATSTG state{};
    if(FAILED(GetHGlobalFromStream(stream,&encoded))||FAILED(stream->Stat(&state,STATFLAG_NONAME))||state.cbSize.HighPart) {stream->Release();return false;}
    PWSTR pictures{};
    if(FAILED(SHGetKnownFolderPath(FOLDERID_Pictures,KF_FLAG_DEFAULT,nullptr,&pictures))){stream->Release();return false;}
    const auto directory=std::filesystem::path(pictures)/L"Screenshots";CoTaskMemFree(pictures);
    std::error_code error;std::filesystem::create_directories(directory,error);if(error){stream->Release();return false;}
    SYSTEMTIME now{};GetLocalTime(&now);static std::atomic<unsigned> serial{};
    wchar_t name[128]{};
    std::swprintf(name,std::size(name),L"LogiOptionsOpen-%04u%02u%02u-%02u%02u%02u-%03u-%lu-%u.png",
        now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,now.wMilliseconds,GetCurrentProcessId(),++serial);
    const auto path=directory/name;
    if(!context.active()){stream->Release();return false;}
    // CREATE_NEW prevents overwriting even if two invocations use the same name.
    const auto output=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(output==INVALID_HANDLE_VALUE){stream->Release();return false;}
    const auto data=GlobalLock(encoded);DWORD written{};
    const bool saved=data&&WriteFile(output,data,state.cbSize.LowPart,&written,nullptr)&&written==state.cbSize.LowPart;
    if(data)GlobalUnlock(encoded);
    CloseHandle(output);stream->Release();
    if(!saved){std::filesystem::remove(path,error);return false;}
    std::wprintf(L"Screen capture saved: %ls\n",path.c_str());return true;
}
}
bool capture(Area area,Destination destination,const ActionContext& context) {
    if(!context.active())return false;
    Frame frame;if(!frame.read(area,context)||!context.active())return false;
    return destination==Destination::clipboard?clipboard(frame,context):file(frame,context);
}
}
