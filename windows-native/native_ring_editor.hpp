#pragma once

#include "ring_visual.hpp"
#include "native_ring_icon_library.hpp"
#include <commdlg.h>
#include <commctrl.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ring_editor {
struct Result {RingIcon icon;unsigned slots{};};
struct Editor {
    HWND window{},owner{},apply{},text{},search{},font_list{},image_hint{};HFONT font{};
    RingIcon original,draft;std::vector<RingIcon> undo,redo;
    RingFonts fonts;std::vector<std::wstring> font_names;
    ring_icon_library::Rasterizer rasterizer;
    std::vector<ring_icon_library::Item> library;
    std::vector<std::size_t> filtered_icons;
    std::array<std::shared_ptr<Gdiplus::Image>,24> thumbnails;
    std::unordered_map<std::wstring,std::shared_ptr<Gdiplus::Image>> thumbnail_cache;
    std::array<std::filesystem::path,24> thumbnail_paths;
    std::size_t library_page{};
    std::array<std::wstring,8> slot_names;
    unsigned current_slot{},selected_slots{};bool selecting{},font_open{};
    int drag{};POINT drag_start{};RingIcon drag_original;
    std::unique_ptr<Gdiplus::Image> image;
    std::optional<Result> result;bool done{},loading{};int dpi{96};
    HBRUSH brush{CreateSolidBrush(RGB(25,25,25))},field{CreateSolidBrush(RGB(42,42,42))};
    int px(int n)const{return MulDiv(n,dpi,96);}
    ~Editor(){DeleteObject(font);DeleteObject(brush);DeleteObject(field);}
};
inline constexpr std::wstring_view glyphs[]{L"keyboard",L"folder",L"play",L"stop",L"volume",L"mute",L"next",L"previous",L"capture",L"desktop",L"flow",L"lock",L"note",L"buttons",L"pointer",L"search",L"settings",L"smart",L"ring",L"home",L"back",L"undo",L"redo",L"grid"};
inline std::wstring text(HWND h){const int size=GetWindowTextLengthW(h);std::wstring out(size+1,L'\0');GetWindowTextW(h,out.data(),size+1);out.resize(size);return out;}
inline void image(Editor& e){e.image.reset();if(!e.draft.image.empty()){auto value=std::make_unique<Gdiplus::Image>(e.draft.image.c_str());if(value->GetLastStatus()==Gdiplus::Ok)e.image=std::move(value);}}
inline void update(Editor& e){EnableWindow(e.apply,!e.selecting&&e.draft!=e.original);EnableWindow(GetDlgItem(e.window,10),!e.undo.empty());EnableWindow(GetDlgItem(e.window,11),!e.redo.empty());InvalidateRect(e.window,nullptr,FALSE);}
inline void remember(Editor& e){e.undo.push_back(e.draft);e.redo.clear();}
inline void transform(RingIcon&icon,const RingIcon&start,int kind,int dx,int dy) {
    if(kind==1){icon.offset_x=std::clamp(start.offset_x+dx*100/256,-100,100);icon.offset_y=std::clamp(start.offset_y+dy*100/256,-100,100);}
    else icon.scale=std::clamp(start.scale+(dx+dy)*100/256,25,200);
}
inline int CALLBACK font_name(const LOGFONTW*logical,const TEXTMETRICW*,DWORD,LPARAM context) {
    auto&names=*reinterpret_cast<std::vector<std::wstring>*>(context);
    if(logical->lfFaceName[0]!=L'@'&&std::ranges::find(names,std::wstring(logical->lfFaceName))==names.end())names.emplace_back(logical->lfFaceName);
    return 1;
}
inline void select_slots(Editor&e,bool show) {
    e.selecting=show;
    for(int id=10;id<=13;++id)ShowWindow(GetDlgItem(e.window,id),show?SW_HIDE:SW_SHOW);
    if(e.image_hint)ShowWindow(e.image_hint,show?SW_HIDE:SW_SHOW);
    EnableWindow(GetDlgItem(e.window,3),!show);
    for(int id=200;id<=209;++id)ShowWindow(GetDlgItem(e.window,id),show?SW_SHOW:SW_HIDE);
    if(show)for(int id=200;id<=209;++id)SetWindowPos(GetDlgItem(e.window,id),HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    if(show)EnableWindow(GetDlgItem(e.window,209),e.selected_slots!=0);
    update(e);
}
inline HWND control(Editor&e,const wchar_t*type,const wchar_t*title,DWORD style,int id,int x,int y,int w,int h){
    const auto window=CreateWindowExW(0,type,title,WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS|style,e.px(x),e.px(y),e.px(w),e.px(h),e.window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(window,WM_SETFONT,reinterpret_cast<WPARAM>(e.font),TRUE);return window;
}
inline LRESULT CALLBACK keep_owner_draw(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR) {
    if(message==BM_SETSTYLE)wp=(wp&~BS_TYPEMASK)|BS_OWNERDRAW;
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,keep_owner_draw,id);
    return DefSubclassProc(window,message,wp,lp);
}
inline HWND button(Editor&e,const wchar_t*title,int id,int x,int y,int w,int h){
    const auto window=control(e,L"BUTTON",title,BS_OWNERDRAW|WS_TABSTOP,id,x,y,w,h);SetWindowSubclass(window,keep_owner_draw,1,0);return window;
}
inline void label(Editor&e,const wchar_t*title,int x,int y,int w,int h){control(e,L"STATIC",title,SS_LEFT,0,x,y,w,h);}
inline void fill_library(Editor&e) {
    e.filtered_icons.clear();const auto query=text(e.search);
    const auto count=e.library.empty()?std::size(glyphs):e.library.size();
    for(std::size_t i=0;i<count;++i){const auto name=e.library.empty()?std::wstring(glyphs[i]):e.library[i].name;if(query.empty()||StrStrIW(name.c_str(),query.c_str()))e.filtered_icons.push_back(i);}
    const auto pages=std::max<std::size_t>(1,(e.filtered_icons.size()+23)/24);e.library_page=std::min(e.library_page,pages-1);
    for(std::size_t slot=0;slot<24;++slot){const auto control=GetDlgItem(e.window,100+static_cast<int>(slot));const auto at=e.library_page*24+slot;e.thumbnails[slot].reset();e.thumbnail_paths[slot].clear();
        if(at>=e.filtered_icons.size()){ShowWindow(control,SW_HIDE);continue;}
        const auto index=e.filtered_icons[at];const auto name=e.library.empty()?std::wstring(glyphs[index]):e.library[index].name;SetWindowTextW(control,name.c_str());
        if(!e.library.empty()){e.thumbnail_paths[slot]=e.rasterizer.rasterize(e.library[index],ring_icon_library::cache_directory());if(!e.thumbnail_paths[slot].empty()){const auto key=e.thumbnail_paths[slot].wstring();auto& cached=e.thumbnail_cache[key];if(!cached){auto image=std::make_shared<Gdiplus::Image>(key.c_str());if(image->GetLastStatus()==Gdiplus::Ok)cached=std::move(image);}e.thumbnails[slot]=cached;}}
        ShowWindow(control,SW_SHOW);InvalidateRect(control,nullptr,FALSE);
    }
    EnableWindow(GetDlgItem(e.window,14),e.library_page>0);EnableWindow(GetDlgItem(e.window,15),e.library_page+1<pages);SetWindowTextW(GetDlgItem(e.window,16),(std::to_wstring(e.library_page+1)+L" / "+std::to_wstring(pages)).c_str());
}
inline void sync_numbers(Editor&e){e.loading=true;
    for(const auto& [id,value]:{std::pair{30,e.draft.font_size},std::pair{31,e.draft.scale},std::pair{32,e.draft.offset_x},std::pair{33,e.draft.offset_y}})SetWindowTextW(GetDlgItem(e.window,id),std::to_wstring(value).c_str());
    e.loading=false;
}
inline void sync(Editor&e){sync_numbers(e);e.loading=true;SetWindowTextW(e.text,e.draft.text.c_str());SetWindowTextW(GetDlgItem(e.window,41),e.draft.font_family.c_str());
    e.loading=false;image(e);update(e);
}
inline void pick_color(Editor&e,unsigned& value){COLORREF colors[16]{};CHOOSECOLORW choose{};choose.lStructSize=sizeof(choose);choose.hwndOwner=e.window;choose.rgbResult=ring_rgb(value);choose.lpCustColors=colors;choose.Flags=CC_FULLOPEN|CC_RGBINIT;if(ChooseColorW(&choose)){remember(e);value=(GetRValue(choose.rgbResult)<<16)|(GetGValue(choose.rgbResult)<<8)|GetBValue(choose.rgbResult);update(e);}}
inline LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp){
    auto*e=reinterpret_cast<Editor*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){e=static_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);e->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(e));}
    if(!e)return DefWindowProcW(window,message,wp,lp);
    switch(message){
    case WM_CREATE:{
        e->loading=true;
        label(*e,L"图标库",24,24,220,26);e->search=control(*e,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,40,24,66,220,30);
        SendMessageW(e->search,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"搜索图标"));
        label(*e,L"Default Icons",24,114,220,22);
        for(int i=0;i<static_cast<int>(std::size(glyphs));++i)button(*e,std::wstring(glyphs[i]).c_str(),100+i,24+(i%4)*55,150+(i/4)*52,46,44);
        button(*e,L"‹",14,24,475,40,32);button(*e,L"›",15,204,475,40,32);control(*e,L"STATIC",L"",SS_CENTER,16,77,480,120,26);
        label(*e,L"编辑图标",300,24,450,26);
        button(*e,L"撤销",10,310,384,74,32);button(*e,L"重做",11,390,384,74,32);button(*e,L"重置",12,470,384,74,32);button(*e,L"浏览…",13,578,384,110,32);
        e->image_hint=control(*e,L"STATIC",L"PNG / JPEG · 最大 150 KB",SS_LEFT,0,310,430,420,24);
        label(*e,L"背景颜色",790,24,160,24);button(*e,L"选择",20,1065,20,94,32);button(*e,L"透明",21,963,20,94,32);
        label(*e,L"布局",790,68,260,24);
        const wchar_t*layouts[]{L"图标上",L"文字上",L"居中",L"全尺寸"};for(int i=0;i<4;++i)button(*e,layouts[i],50+i,790+i*94,100,86,32);
        button(*e,L"图标显示",22,790,150,120,32);button(*e,L"图标颜色",23,930,150,145,32);
        button(*e,L"文字显示",24,790,199,120,32);button(*e,L"文字颜色",25,930,199,145,32);
        e->text=control(*e,L"EDIT",e->draft.text.c_str(),ES_AUTOHSCROLL|WS_TABSTOP,29,790,246,369,32);SendMessageW(e->text,EM_SETLIMITTEXT,24,0);
        button(*e,e->draft.font_family.c_str(),41,790,292,369,32);
        label(*e,L"字体大小",790,342,110,24);control(*e,L"EDIT",L"27",ES_NUMBER|WS_TABSTOP,30,902,337,72,30);button(*e,L"粗体",26,1010,337,149,30);
        label(*e,L"缩放 %",790,389,110,24);control(*e,L"EDIT",L"100",ES_NUMBER|WS_TABSTOP,31,902,384,72,30);
        label(*e,L"位置 X / Y",790,437,110,24);control(*e,L"EDIT",L"0",WS_TABSTOP,32,902,432,72,30);control(*e,L"EDIT",L"0",WS_TABSTOP,33,990,432,72,30);
        button(*e,L"取消",1,790,520,100,36);button(*e,L"应用于多个",3,906,520,139,36);e->apply=button(*e,L"应用",2,1061,520,100,36);
        for(int i=0;i<8;++i){const auto name=std::to_wstring(i+1)+L"  "+(e->slot_names[i].empty()?L"空槽位":e->slot_names[i]);button(*e,name.c_str(),200+i,300+(i%2)*216,148+(i/2)*52,204,44);}
        button(*e,L"取消选择",208,360,376,140,36);button(*e,L"应用到所选槽位",209,514,376,200,36);select_slots(*e,false);
        static std::vector<std::wstring> cached_font_names;
        if(cached_font_names.empty()){
            const auto dc=GetDC(window);LOGFONTW logical{};logical.lfCharSet=DEFAULT_CHARSET;EnumFontFamiliesExW(dc,&logical,font_name,reinterpret_cast<LPARAM>(&cached_font_names),0);ReleaseDC(window,dc);
            std::ranges::sort(cached_font_names);
        }
        e->font_names=cached_font_names;
        if(std::ranges::find(e->font_names,e->draft.font_family)==e->font_names.end()){e->font_names.push_back(e->draft.font_family);std::ranges::sort(e->font_names);}
        e->font_list=control(*e,L"LISTBOX",L"",LBS_NOTIFY|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|WS_VSCROLL|WS_BORDER,42,790,326,369,156);SendMessageW(e->font_list,LB_SETITEMHEIGHT,0,e->px(26));
        SendMessageW(e->font_list,WM_SETREDRAW,FALSE,0);
        for(const auto&name:e->font_names)SendMessageW(e->font_list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));
        SendMessageW(e->font_list,WM_SETREDRAW,TRUE,0);
        ShowWindow(e->font_list,SW_HIDE);
        static std::vector<ring_icon_library::Item> cached_library=ring_icon_library::enumerate();
        e->library=cached_library;fill_library(*e);sync(*e);e->loading=false;return 0;
    }
    case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:{auto dc=reinterpret_cast<HDC>(wp);SetTextColor(dc,RGB(251,251,251));SetBkColor(dc,message!=WM_CTLCOLORSTATIC?RGB(42,42,42):RGB(25,25,25));return reinterpret_cast<LRESULT>(message!=WM_CTLCOLORSTATIC?e->field:e->brush);}
    case WM_DRAWITEM:{const auto& item=*reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if(item.CtlID==42){if(item.itemID>=e->font_names.size())return TRUE;FillRect(item.hDC,&item.rcItem,e->field);SetBkMode(item.hDC,TRANSPARENT);SetTextColor(item.hDC,item.itemState&ODS_SELECTED?RGB(0,234,208):RGB(251,251,251));auto rect=item.rcItem;rect.left+=e->px(10);DrawTextW(item.hDC,e->font_names[item.itemID].c_str(),-1,&rect,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);return TRUE;}
        const bool selected=(item.CtlID>=50&&item.CtlID<54&&e->draft.layout==static_cast<int>(item.CtlID)-50)||(item.CtlID==22&&e->draft.show_icon)||(item.CtlID==24&&e->draft.show_text)||(item.CtlID==26&&e->draft.bold)||(item.CtlID>=200&&item.CtlID<208&&(e->selected_slots&(1u<<(item.CtlID-200))));
        // Antialiased FillRectangle at (0,0) leaves partially covered top/left
        // pixels over the system's light button background. Paint an opaque base.
        const bool disabled=item.itemState&ODS_DISABLED;
        const auto old_fill=SetDCBrushColor(item.hDC,selected?RGB(0,234,208):RGB(42,42,42));
        FillRect(item.hDC,&item.rcItem,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));SetDCBrushColor(item.hDC,old_fill);
        if(item.CtlID>=100&&item.CtlID<124){const auto slot=item.CtlID-100;const auto at=e->library_page*24+slot;
            Gdiplus::Graphics graphics(item.hDC);graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            if(e->thumbnails[slot])graphics.DrawImage(e->thumbnails[slot].get(),e->px(7),e->px(6),e->px(32),e->px(32));
            else if(e->library.empty()&&at<e->filtered_icons.size())draw_native_icon(graphics,glyphs[e->filtered_icons[at]],e->px(11),e->px(10),e->px(24),RGB(251,251,251));
        }
        else {SetBkMode(item.hDC,TRANSPARENT);SetTextColor(item.hDC,disabled?RGB(105,105,105):selected?RGB(0,0,0):RGB(251,251,251));const auto old=SelectObject(item.hDC,e->font);auto rect=item.rcItem;const auto value=text(item.hwndItem);DrawTextW(item.hDC,value.c_str(),-1,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);SelectObject(item.hDC,old);}return TRUE;}
    case WM_PAINT:{PAINTSTRUCT paint{};const auto dc=BeginPaint(window,&paint);RECT area{};GetClientRect(window,&area);FillRect(dc,&area,e->brush);Gdiplus::Graphics graphics(dc);Gdiplus::Pen divider(Gdiplus::Color(255,55,55,55));graphics.DrawLine(&divider,e->px(270),e->px(20),e->px(270),e->px(494));graphics.DrawLine(&divider,e->px(765),e->px(20),e->px(765),e->px(494));
        Gdiplus::SolidBrush canvas(Gdiplus::Color(255,0,0,0));graphics.FillEllipse(&canvas,e->px(372),e->px(92),e->px(256),e->px(256));draw_ring_visual(graphics,e->draft,e->image.get(),e->px(372),e->px(92),e->px(256),&e->fonts);
        Gdiplus::Pen selection(Gdiplus::Color(255,0,234,208));selection.SetDashStyle(Gdiplus::DashStyleDash);graphics.DrawRectangle(&selection,e->px(372),e->px(92),e->px(256),e->px(256));Gdiplus::SolidBrush handle(Gdiplus::Color(255,0,234,208));graphics.FillRectangle(&handle,e->px(622),e->px(342),e->px(12),e->px(12));
        if(e->selecting){Gdiplus::SolidBrush overlay(Gdiplus::Color(255,25,25,25));graphics.FillRectangle(&overlay,e->px(284),e->px(90),e->px(458),e->px(348));SetTextColor(dc,RGB(251,251,251));SetBkMode(dc,TRANSPARENT);const auto old=SelectObject(dc,e->font);RECT title{e->px(300),e->px(105),e->px(730),e->px(136)};DrawTextW(dc,L"选择要应用图标的槽位",-1,&title,DT_LEFT|DT_VCENTER|DT_SINGLELINE);SelectObject(dc,old);}
        EndPaint(window,&paint);return 0;}
    case WM_LBUTTONDOWN:{const int x=MulDiv(GET_X_LPARAM(lp),96,e->dpi),y=MulDiv(GET_Y_LPARAM(lp),96,e->dpi);if(!e->selecting&&x>=372&&x<=634&&y>=92&&y<=354){remember(*e);e->drag=x>=617&&y>=337?2:1;e->drag_start={x,y};e->drag_original=e->draft;SetCapture(window);}return 0;}
    case WM_MOUSEMOVE:if(e->drag){transform(e->draft,e->drag_original,e->drag,MulDiv(GET_X_LPARAM(lp),96,e->dpi)-e->drag_start.x,MulDiv(GET_Y_LPARAM(lp),96,e->dpi)-e->drag_start.y);sync_numbers(*e);update(*e);}return 0;
    case WM_LBUTTONUP:if(e->drag){e->drag=0;ReleaseCapture();update(*e);}return 0;
    case WM_CAPTURECHANGED:e->drag=0;return 0;
    case WM_COMMAND:{const int id=LOWORD(wp),event=HIWORD(wp);if(e->loading)return 0;
        if(id==42&&event==LBN_SELCHANGE){const auto index=SendMessageW(e->font_list,LB_GETCURSEL,0,0);if(index>=0&&static_cast<std::size_t>(index)<e->font_names.size()){remember(*e);e->draft.font_family=e->font_names[index];SetWindowTextW(GetDlgItem(window,41),e->draft.font_family.c_str());e->font_open=false;ShowWindow(e->font_list,SW_HIDE);update(*e);}return 0;}
        if(event==EN_CHANGE&&id==40){e->library_page=0;fill_library(*e);return 0;}
        if(event==EN_CHANGE&&(id==29||(id>=30&&id<=33))){if(id==29){remember(*e);e->draft.text=text(e->text);}else{const auto value=text(GetDlgItem(window,id));wchar_t*end{};const auto n=wcstol(value.c_str(),&end,10);if(value.empty()||*end)return 0;const int low=id==30?8:id==31?25:-100,high=id==30?96:id==31?200:100;if(n<low||n>high)return 0;remember(*e);(id==30?e->draft.font_size:id==31?e->draft.scale:id==32?e->draft.offset_x:e->draft.offset_y)=static_cast<int>(n);}update(*e);return 0;}
        if(event!=BN_CLICKED)return 0;
        if(id==1){e->done=true;return 0;}if(id==2){e->result=Result{e->draft,1u<<e->current_slot};e->done=true;return 0;}
        if(id==3){e->selected_slots=1u<<e->current_slot;select_slots(*e,true);return 0;}
        if(id>=200&&id<208){e->selected_slots^=1u<<(id-200);EnableWindow(GetDlgItem(window,209),e->selected_slots!=0);InvalidateRect(GetDlgItem(window,id),nullptr,FALSE);return 0;}
        if(id==208){select_slots(*e,false);return 0;}if(id==209){if(e->selected_slots){e->result=Result{e->draft,e->selected_slots};e->done=true;}return 0;}
        if(id==41){e->font_open=!e->font_open;ShowWindow(e->font_list,e->font_open?SW_SHOW:SW_HIDE);if(e->font_open){SetWindowPos(e->font_list,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);SetFocus(e->font_list);}return 0;}
        if(id==14||id==15){if(id==14&&e->library_page)--e->library_page;else if(id==15)++e->library_page;fill_library(*e);return 0;}
        if(id>=100&&id<124){const auto slot=id-100;const auto at=e->library_page*24+slot;if(at>=e->filtered_icons.size())return 0;remember(*e);
            if(e->library.empty()){e->draft.glyph=glyphs[e->filtered_icons[at]];e->draft.image.clear();}
            else{if(e->thumbnail_paths[slot].empty()){MessageBoxW(window,L"此图标无法读取。",L"图标",MB_OK);return 0;}e->draft.glyph=L"original-svg";e->draft.image=e->thumbnail_paths[slot].wstring();}
            image(*e);update(*e);return 0;
        }
        if(id==10||id==11){auto&from=id==10?e->undo:e->redo;auto&to=id==10?e->redo:e->undo;if(!from.empty()){to.push_back(e->draft);e->draft=from.back();from.pop_back();sync(*e);}return 0;}
        if(id==12){remember(*e);e->draft=e->original;sync(*e);return 0;}
        if(id==13){wchar_t path[4096]{};OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=window;dialog.lpstrFile=path;dialog.nMaxFile=std::size(path);dialog.lpstrFilter=L"图像 (*.png;*.jpg;*.jpeg)\0*.png;*.jpg;*.jpeg\0";dialog.Flags=OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR;if(GetOpenFileNameW(&dialog)){std::error_code error;const auto size=std::filesystem::file_size(path,error);Gdiplus::Image candidate(path);if(error||size>150*1024||candidate.GetLastStatus()!=Gdiplus::Ok){MessageBoxW(window,L"请选择不超过 150 KB 的 PNG 或 JPEG 图像。",L"图像无法使用",MB_OK);return 0;}remember(*e);e->draft.image=path;sync(*e);}return 0;}
        if(id==20){unsigned rgb=e->draft.background.value_or(0);const auto before=e->undo.size();pick_color(*e,rgb);if(e->undo.size()!=before)e->draft.background=rgb;update(*e);return 0;}
        if(id==23||id==25){pick_color(*e,id==23?e->draft.color:e->draft.text_color);return 0;}
        remember(*e);if(id==21)e->draft.background.reset();else if(id==22)e->draft.show_icon=!e->draft.show_icon;else if(id==24)e->draft.show_text=!e->draft.show_text;else if(id==26)e->draft.bold=!e->draft.bold;else if(id>=50&&id<54)e->draft.layout=id-50;update(*e);return 0;
    }
    case WM_CLOSE:e->done=true;return 0;
    }return DefWindowProcW(window,message,wp,lp);
}
inline std::optional<Result> edit(HWND owner,RingIcon icon,std::array<std::wstring,8> names,unsigned selected){
    Editor e;e.owner=owner;e.original=e.draft=std::move(icon);e.slot_names=std::move(names);e.current_slot=selected;e.dpi=GetDpiForWindow(owner);e.font=CreateFontW(-e.px(16),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.lpszClassName=L"LogiNativeRingIconEditor";RegisterClassW(&cls);
    RECT owner_rect{};GetWindowRect(owner,&owner_rect);const int width=e.px(1185),height=e.px(577);
    const auto window=CreateWindowExW(0,cls.lpszClassName,L"编辑图标",WS_POPUP|WS_CLIPCHILDREN,(owner_rect.left+owner_rect.right-width)/2,(owner_rect.top+owner_rect.bottom-height)/2,width,height,owner,nullptr,cls.hInstance,&e);if(!window)return {};
    const BOOL dark=TRUE;DwmSetWindowAttribute(window,20,&dark,sizeof(dark));const DWORD corners=2;DwmSetWindowAttribute(window,33,&corners,sizeof(corners));const COLORREF border=RGB(53,53,53);DwmSetWindowAttribute(window,34,&border,sizeof(border));
    EnableWindow(owner,FALSE);ShowWindow(window,SW_SHOW);SetForegroundWindow(window);MSG message{};
    while(!e.done&&GetMessageW(&message,nullptr,0,0)>0){
        if(message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE){
            if(e.selecting){select_slots(e,false);continue;}
            if(e.font_open){e.font_open=false;ShowWindow(e.font_list,SW_HIDE);continue;}
            e.done=true;break;
        }
        if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
    }
    DestroyWindow(window);EnableWindow(owner,TRUE);SetActiveWindow(owner);return e.result;
}
} // namespace ring_editor
