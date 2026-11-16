#include <windows.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <set>
#include <vector>
#include <stop_token>
static std::filesystem::path fixture;
static std::vector<BYTE> pixels;
static unsigned captures{},desktop_releases{},dc_deletes{},bitmap_deletes{},dpi_sets{},clipboard_opens{},clipboard_closes{},owner_deletes{};
static bool fail_capture{},fail_open{},fail_clipboard{},has_window=true;
static HGLOBAL published{};
static RECT last_capture{};
static std::stop_source* cancel_after_pixels{};
static std::stop_source* cancel_before_publish{};
static std::stop_source* cancel_before_file{};
DPI_AWARENESS_CONTEXT WINAPI fake_dpi(DPI_AWARENESS_CONTEXT context){assert(context==DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2||context==DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);++dpi_sets;return DPI_AWARENESS_CONTEXT_SYSTEM_AWARE;}
int WINAPI fake_metrics(int index){switch(index){case SM_XVIRTUALSCREEN:return -4;case SM_YVIRTUALSCREEN:return 0;case SM_CXVIRTUALSCREEN:return 4;case SM_CYVIRTUALSCREEN:return 3;default:assert(false);return 0;}}
HWND WINAPI fake_front(){return has_window?reinterpret_cast<HWND>(10):nullptr;}
BOOL WINAPI fake_iconic(HWND window){assert(window==reinterpret_cast<HWND>(10));return FALSE;}
HRESULT WINAPI fake_bounds(HWND window,DWORD attribute,PVOID data,DWORD size){assert(window==reinterpret_cast<HWND>(10)&&attribute==DWMWA_EXTENDED_FRAME_BOUNDS&&size==sizeof(RECT));*static_cast<RECT*>(data)={-2,1,1,4};return S_OK;}
BOOL WINAPI reject_window_rect(HWND,LPRECT){assert(false);return FALSE;}
HDC WINAPI fake_dc(HWND window){assert(!window);return reinterpret_cast<HDC>(1);}
HDC WINAPI fake_memory(HDC dc){assert(dc==reinterpret_cast<HDC>(1));return reinterpret_cast<HDC>(2);}
HBITMAP WINAPI fake_dib(HDC dc,const BITMAPINFO* info,UINT usage,void** output,HANDLE section,DWORD offset){assert(dc==reinterpret_cast<HDC>(1)&&usage==DIB_RGB_COLORS&&!section&&!offset&&info->bmiHeader.biHeight<0);pixels.assign(info->bmiHeader.biSizeImage,0);*output=pixels.data();return reinterpret_cast<HBITMAP>(3);}
HGDIOBJ WINAPI fake_select(HDC dc,HGDIOBJ object){assert(dc==reinterpret_cast<HDC>(2));assert(object==reinterpret_cast<HGDIOBJ>(3)||object==reinterpret_cast<HGDIOBJ>(4));return reinterpret_cast<HGDIOBJ>(4);}
BOOL WINAPI fake_blt(HDC destination,int x,int y,int width,int height,HDC source,int left,int top,DWORD mode){
 assert(destination==reinterpret_cast<HDC>(2)&&source==reinterpret_cast<HDC>(1)&&!x&&!y&&mode==(SRCCOPY|CAPTUREBLT));
 ++captures;last_capture={left,top,left+width,top+height};if(fail_capture)return FALSE;
 for(int row=0;row<height;++row)for(int column=0;column<width;++column){const auto i=(row*width+column)*4;pixels[i]=BYTE(20+column);pixels[i+1]=BYTE(30+row);pixels[i+2]=40;}
 if(cancel_after_pixels)cancel_after_pixels->request_stop();return TRUE;
}
BOOL WINAPI fake_delete_object(HGDIOBJ bitmap){assert(bitmap==reinterpret_cast<HGDIOBJ>(3));++bitmap_deletes;return TRUE;}
BOOL WINAPI fake_delete_dc(HDC dc){assert(dc==reinterpret_cast<HDC>(2));++dc_deletes;return TRUE;}
int WINAPI fake_release(HWND window,HDC dc){assert(!window&&dc==reinterpret_cast<HDC>(1));++desktop_releases;return 1;}
HWND WINAPI fake_owner(DWORD,LPCWSTR name,LPCWSTR,DWORD style,int,int,int,int,HWND parent,HMENU,HINSTANCE,LPVOID){assert(std::wstring(name)==L"STATIC"&&style==0&&parent==HWND_MESSAGE);return reinterpret_cast<HWND>(20);}
BOOL WINAPI fake_destroy(HWND owner){assert(owner==reinterpret_cast<HWND>(20));++owner_deletes;return TRUE;}
BOOL WINAPI fake_open(HWND owner){assert(owner==reinterpret_cast<HWND>(20));++clipboard_opens;if(cancel_before_publish)cancel_before_publish->request_stop();return !fail_open;}
BOOL WINAPI fake_empty(){return TRUE;}
HANDLE WINAPI fake_publish(UINT format,HANDLE memory){assert(format==CF_DIB&&!published);if(fail_clipboard)return nullptr;published=static_cast<HGLOBAL>(memory);return memory;}
BOOL WINAPI fake_close(){++clipboard_closes;return TRUE;}
HRESULT WINAPI fake_pictures(REFKNOWNFOLDERID id,DWORD flags,HANDLE token,PWSTR* output){assert(id==FOLDERID_Pictures&&flags==KF_FLAG_DEFAULT&&!token);const auto text=fixture.wstring();*output=static_cast<PWSTR>(CoTaskMemAlloc((text.size()+1)*sizeof(wchar_t)));std::memcpy(*output,text.c_str(),(text.size()+1)*sizeof(wchar_t));if(cancel_before_file)cancel_before_file->request_stop();return S_OK;}
#define SetThreadDpiAwarenessContext fake_dpi
#define GetSystemMetrics fake_metrics
#define GetForegroundWindow fake_front
#define IsIconic fake_iconic
#define DwmGetWindowAttribute fake_bounds
#define GetWindowRect reject_window_rect
#define GetDC fake_dc
#define CreateCompatibleDC fake_memory
#define CreateDIBSection fake_dib
#define SelectObject fake_select
#define BitBlt fake_blt
#define DeleteObject fake_delete_object
#define DeleteDC fake_delete_dc
#define ReleaseDC fake_release
#define CreateWindowExW fake_owner
#define DestroyWindow fake_destroy
#define OpenClipboard fake_open
#define EmptyClipboard fake_empty
#define SetClipboardData fake_publish
#define CloseClipboard fake_close
#define SHGetKnownFolderPath fake_pictures
#include "../screen_capture.cpp"
int main(int argc,char** argv) {
 assert(argc==2);fixture=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(fixture/L"Screenshots");
 std::set<std::filesystem::path> existing;for(const auto& entry:std::filesystem::directory_iterator(fixture/L"Screenshots"))existing.insert(entry.path());
 using screen_capture::Area;using screen_capture::Destination;using screen_capture::capture;
 assert(capture(Area::screen,Destination::file));assert(last_capture.left==-4&&last_capture.top==0&&last_capture.right==0&&last_capture.bottom==3);
 assert(capture(Area::window,Destination::file));assert(last_capture.left==-2&&last_capture.top==1&&last_capture.right==0&&last_capture.bottom==3);
 unsigned files{};screen_capture::GdiPlus gdiplus;
 for(const auto& entry:std::filesystem::directory_iterator(fixture/L"Screenshots"))if(!existing.contains(entry.path())) {
    Gdiplus::Bitmap bitmap(entry.path().c_str());assert(bitmap.GetLastStatus()==Gdiplus::Ok);
    assert((bitmap.GetWidth()==4&&bitmap.GetHeight()==3)||(bitmap.GetWidth()==2&&bitmap.GetHeight()==2));
    Gdiplus::Color first,last;assert(bitmap.GetPixel(0,0,&first)==Gdiplus::Ok);assert(first.GetR()==40&&first.GetG()==30&&first.GetB()==20&&first.GetA()==255);
    assert(bitmap.GetPixel(bitmap.GetWidth()-1,bitmap.GetHeight()-1,&last)==Gdiplus::Ok);assert(last.GetG()==30+bitmap.GetHeight()-1&&last.GetB()==20+bitmap.GetWidth()-1);++files;
 }
 assert(files==2);
 for(const auto area:{Area::screen,Area::window}) {
    assert(capture(area,Destination::clipboard)&&published);auto data=static_cast<BYTE*>(GlobalLock(published));
    const auto& header=*reinterpret_cast<BITMAPINFOHEADER*>(data);assert(header.biWidth==(area==Area::screen?4:2)&&header.biHeight==(area==Area::screen?-3:-2)&&header.biBitCount==32);
    assert(data[sizeof(BITMAPINFOHEADER)]==20&&data[sizeof(BITMAPINFOHEADER)+2]==40);GlobalUnlock(published);GlobalFree(published);published=nullptr;
 }
 fail_open=true;assert(!capture(Area::screen,Destination::clipboard)&&!published);fail_open=false;
 fail_clipboard=true;assert(!capture(Area::screen,Destination::clipboard)&&!published);fail_clipboard=false;
 fail_capture=true;const auto opens=clipboard_opens;assert(!capture(Area::screen,Destination::clipboard)&&clipboard_opens==opens);fail_capture=false;
 has_window=false;assert(!capture(Area::window,Destination::file));
 assert(captures==7&&desktop_releases==7&&dc_deletes==7&&bitmap_deletes==7&&dpi_sets==16);
 assert(clipboard_opens==4&&clipboard_closes==3&&owner_deletes==4);
 // Cancellation after a potentially slow read/encode must not publish content.
 const auto old_captures=captures,old_opens=clipboard_opens;
 std::stop_source stopped;stopped.request_stop();
 assert(!capture(Area::screen,Destination::clipboard,{{},0,stopped.get_token()})&&captures==old_captures);
 std::stop_source after_read;cancel_after_pixels=&after_read;
 assert(!capture(Area::screen,Destination::clipboard,{{},0,after_read.get_token()})&&clipboard_opens==old_opens&&!published);
 cancel_after_pixels=nullptr;
 std::stop_source before_clipboard;cancel_before_publish=&before_clipboard;
 assert(!capture(Area::screen,Destination::clipboard,{{},0,before_clipboard.get_token()})&&!published);
 cancel_before_publish=nullptr;
 const auto count_files=[&]{return std::distance(std::filesystem::directory_iterator(fixture/L"Screenshots"),std::filesystem::directory_iterator{});};
 const auto prior_files=count_files();std::stop_source before_file;cancel_before_file=&before_file;
 assert(!capture(Area::screen,Destination::file,{{},0,before_file.get_token()})&&count_files()==prior_files);cancel_before_file=nullptr;
 assert(desktop_releases==captures&&dc_deletes==captures&&bitmap_deletes==captures&&clipboard_closes+1==clipboard_opens);
 std::puts("Production capture: synthetic desktop/window rectangles, real opaque PNG pixel round-trip, CF_DIB ownership, failure cleanup and DPI context restoration passed; no real screen/clipboard accessed.");
}
