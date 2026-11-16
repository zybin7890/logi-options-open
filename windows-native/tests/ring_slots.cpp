#include <cassert>
#include <cstdio>
#include <deque>
#include "../ring_profile_package.hpp"
#include "reject_effects.hpp"
#include "../remap.cpp"
#include <objidl.h>
#include <gdiplus.h>
#include "../ring_visual.hpp"
#include "../native_ring_editor.hpp"

namespace {
std::deque<int> choices;
std::vector<std::array<std::wstring, 8>> displays;
std::vector<bool> submenus;
std::vector<std::array<RingIcon,8>> icon_displays;
bool parse(const char* text, Mappings& result) {
    std::istringstream input(text);
    return parse_mappings(input, result);
}
}

int choose_ring_action(std::span<const std::wstring> labels, HANDLE, bool submenu,std::span<const RingIcon> icons, float) {
    assert(labels.size() == 8);
    std::array<std::wstring, 8> shown;
    std::ranges::copy(labels, shown.begin());
    displays.push_back(std::move(shown));
    submenus.push_back(submenu);
    assert(icons.size()==8);std::array<RingIcon,8> shown_icons;std::ranges::copy(icons,shown_icons.begin());icon_displays.push_back(std::move(shown_icons));
    assert(!choices.empty());
    const int value = choices.front();
    choices.pop_front();
    return value;
}

int main() {
    Mappings sparse;
    assert(parse("[global]\n00C3=Ring\n[ring]\n@empty.1=\nPlay=PlayPause\n@empty.3=\nTools=Folder(Tools)\n[ring/Tools]\n", sparse));
    assert(sparse.ring.size() == 4 && sparse.ring[0].label.empty());
    assert(sparse.ring[0].binding.actions.empty());
    assert(sparse.ring[1].label == L"Play" && sparse.ring[1].binding.actions[0].keys[0] == VK_MEDIA_PLAY_PAUSE);
    assert(sparse.folders.size() == 1 && sparse.folders[0].items.empty());
    choices = {1};
    assert(choose_item(sparse, nullptr) == &sparse.ring[1]);
    assert(displays.back()[0].empty() && displays.back()[1] == L"Play" && displays.back()[7].empty());
    choices = {3, ring_back, 1};
    assert(choose_item(sparse, nullptr) == &sparse.ring[1]);
    assert(submenus[submenus.size()-2]);
    assert(std::ranges::all_of(displays[displays.size()-2], [](const auto& label) { return label.empty(); }));
    choices = {0};
    assert(!choose_item(sparse, nullptr));
    choices = {7};
    assert(!choose_item(sparse, nullptr));
    choices = {3, 7};
    assert(!choose_item(sparse, nullptr));

    Mappings empty;
    assert(parse("[global]\n00C3=Ring\n[ring]\n", empty));
    assert(empty.ring.size() == 8);
    choices = {-1};
    assert(!choose_item(empty, nullptr));
    Mappings missing;
    assert(!parse("[global]\n00C3=Ring\n", missing));
    for (const auto bad : {
        "[ring]\n@empty.2=\n",
        "[ring]\n@empty.0=\n",
        "[ring]\n@empty.01=\n",
        "[ring]\n@empty.1=VolumeUp\n",
        "[ring]\n@empty.1=\n@empty.1=\n",
        "[ring]\nLabel=\n",
        "[ring]\nTools=Folder(Missing)\n"}) {
        Mappings invalid;
        assert(!parse(bad, invalid));
    }
    Mappings old;
    assert(parse("[ring]\nOne=VolumeUp\nTwo=None\n", old));
    assert(old.ring.size() == 2 && !old.ring[1].binding.actions.empty());
    Action stop;
    assert(parse_action(L"MediaStop", stop) && stop.keys[0] == VK_MEDIA_STOP);
    Mappings nested;
    assert(parse("[ring]\nTools=Folder(Tools)\nRoot=VolumeUp\n[ring/Tools]\n@empty.1=\nChild=Folder(Child)\n[ring/Child]\n@empty.1=\nRun=PlayPause\nEmpty=Folder(Empty)\n[ring/Empty]\n", nested));
    choices = {0, 1, 1};
    assert(choose_item(nested, nullptr) == &nested.folders[1].items[1]);
    choices = {0, 1, 2, ring_back, ring_back, ring_back, 1};
    assert(choose_item(nested, nullptr) == &nested.ring[1]);
    choices = {0, 1, -1};
    assert(!choose_item(nested, nullptr));
    for (const auto bad : {
        "[ring]\nRoot=Folder(A)\n[ring/A]\nSelf=Folder(A)\n",
        "[ring/A]\nNext=Folder(B)\n[ring/B]\nNext=Folder(A)\n",
        "[ring]\nRoot=Folder(A)\n[ring/A]\nMissing=Folder(B)\n"}) {
        Mappings invalid;
        assert(!parse(bad, invalid));
    }
    Mappings shared;
    assert(parse("[ring]\nOne=Folder(A)\nTwo=Folder(B)\n[ring/A]\nShared=Folder(C)\n[ring/B]\nShared=Folder(C)\n[ring/C]\nRun=PlayPause\n", shared));
    Mappings adaptive;
    assert(parse("[ring]\nGlobal=VolumeUp\n[ring:firefox.exe]\nFirefox=PlayPause\n[ring:empty.exe]\n[ring/Work]\nWork=Mute\n",adaptive));
    choices={0};assert(choose_item(adaptive,nullptr,L"C:\\Apps\\FIREFOX.EXE")==&adaptive.ring_profiles[0].items[0]);
    assert(displays.back()[0]==L"Firefox"&&icon_displays.back()[0].glyph==L"play");
    adaptive.ring_ui.adaptive=false;choices={0};assert(choose_item(adaptive,nullptr,L"firefox.exe")==&adaptive.ring[0]);
    adaptive.ring_ui.adaptive=true;choices={0};assert(!choose_item(adaptive,nullptr,L"empty.exe"));
    adaptive.ring_ui.active[L"firefox.exe"]=L"ring/Work";choices={0};assert(choose_item(adaptive,nullptr,L"firefox.exe")==&adaptive.folders[0].items[0]);
    adaptive.ring_ui.active[L"firefox.exe"]=L"ring/Missing";choices={0};assert(choose_item(adaptive,nullptr,L"firefox.exe")==&adaptive.ring_profiles[0].items[0]);
    adaptive.ring_ui.active[L"global"]=L"ring/Work";adaptive.ring_ui.adaptive=false;choices={0};assert(choose_item(adaptive,nullptr,L"firefox.exe")==&adaptive.folders[0].items[0]);
    Mappings invalid_profile;assert(!parse("[ring]\nGlobal=Mute\n[ring:firefox.exe]\nMissing=Folder(Missing)\n",invalid_profile));

    ring_metadata::Settings settings;settings.adaptive=false;settings.active[L"firefox.exe"]=L"ring/Work";
    settings.profiles.push_back({L"firefox.exe",L"Work profile",L"ring/Work"});
    settings.profiles.push_back({L"global",L"Renamed default",L"ring"});
    RingIcon customized{L"capture",L"C:\\some space\\icon.png",0x12abcd,0x234567};
    customized.text=L"Saved icon";customized.text_color=0xaabbcc;customized.layout=1;customized.font_size=31;customized.scale=125;customized.offset_x=-12;customized.offset_y=13;customized.show_icon=false;customized.show_text=true;customized.bold=true;
    settings.icons[L"ring/Work"][0]=customized;
    const auto encoded=ring_metadata::write(L"# unrelated\n[ring]\nOne=Mute\n",settings);
    const auto decoded=ring_metadata::read(encoded);
    assert(!decoded.adaptive&&decoded.active==settings.active&&decoded.profiles==settings.profiles&&decoded.icons==settings.icons);
    assert(ring_metadata::write(encoded,decoded)==encoded);
    assert(encoded.find(L"# unrelated\n[ring]\nOne=Mute\n")!=encoded.npos);
    adaptive.ring_ui=decoded;choices={0};assert(choose_item(adaptive,nullptr)==&adaptive.ring[0]);
    adaptive.ring_ui.adaptive=true;choices={0};assert(choose_item(adaptive,nullptr,L"firefox.exe")==&adaptive.folders[0].items[0]);
    assert(icon_displays.back()[0]==customized);
    auto broken=encoded;const auto at=broken.find(L" 31 125 -12 13 6");assert(at!=broken.npos);broken.replace(at,16,L" 999 125 -12 13 6");assert(ring_metadata::read(broken).icons.empty());
    assert(!ring_metadata::unhex(L"0041x").has_value());
    {
        auto subset=settings;const auto untouched=subset.icons[L"ring/Work"][0];RingIcon replacement;replacement.text=L"Subset";replacement.font_family=L"Arial";
        ring_metadata::apply_icon(subset,L"ring/Work",(1u<<2)|(1u<<6),replacement);
        assert(subset.icons[L"ring/Work"][0]==untouched&&subset.icons[L"ring/Work"][2]==replacement&&subset.icons[L"ring/Work"][6]==replacement&&subset.icons[L"ring/Work"][3]==RingIcon{});
        assert(ring_metadata::read(ring_metadata::write(L"[ring]\nOne=Mute\n",subset)).icons==subset.icons);
        RingIcon moved;const RingIcon initial;ring_editor::transform(moved,initial,1,64,-32);assert(moved.offset_x==25&&moved.offset_y==-12);
        ring_editor::transform(moved,initial,2,64,64);assert(moved.scale==150);
        ring_editor::transform(moved,initial,2,-1000,-1000);assert(moved.scale==25);
    }
    {
        namespace fs=std::filesystem;
        const auto directory=fs::temp_directory_path()/(L"logi-ring-package-test-"+std::to_wstring(GetCurrentProcessId()));
        fs::create_directories(directory);const auto original=directory/L"original.png";
        Gdiplus::GdiplusStartupInput startup;ULONG_PTR token{};assert(Gdiplus::GdiplusStartup(&token,&startup,nullptr)==Gdiplus::Ok);
        {Gdiplus::Bitmap generated(16,16,PixelFormat32bppARGB);{Gdiplus::Graphics g(&generated);g.Clear(Gdiplus::Color(255,0,180,200));}
            const CLSID png{0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0,0,0xf8,0x1e,0xf3,0x2e}};assert(generated.Save(original.c_str(),&png,nullptr)==Gdiplus::Ok);}
        const auto original_bytes=ring_profile_package::read_image(original);
        ring_metadata::Settings package_settings;package_settings.icons[L"ring"][0].image=original.wstring();
        const auto package=ring_profile_package::encode(L"[ring]\nOne=Mute\n",package_settings);
        fs::remove(original);assert(!fs::exists(original));
        const auto decoded_package=ring_profile_package::decode(package);
        auto imported=ring_metadata::read(decoded_package.source);
        ring_profile_package::materialize(decoded_package,imported,directory/L"imported");
        const auto imported_path=imported.icons.at(L"ring")[0].image;
        assert(imported_path!=original&&fs::exists(imported_path)&&ring_profile_package::read_image(imported_path)==original_bytes);
        {
            Gdiplus::Image loaded(imported_path.c_str());assert(loaded.GetLastStatus()==Gdiplus::Ok&&loaded.GetWidth()>0);
            RingFonts fonts;assert(fonts.family(L"Arial")->GetLastStatus()==Gdiplus::Ok);
            Gdiplus::Bitmap image(96,32,PixelFormat32bppARGB);{Gdiplus::Graphics g(&image);g.Clear(Gdiplus::Color(255,255,0,0));}
            RingIcon appearance;appearance.background=0x123456;appearance.text=L"A";appearance.show_text=true;
            std::array<unsigned long long,4> digests{};
            for(int layout=0;layout<4;++layout){
                appearance.layout=layout;Gdiplus::Bitmap canvas(128,128,PixelFormat32bppARGB);
                {Gdiplus::Graphics g(&canvas);g.Clear(Gdiplus::Color(255,0,0,0));draw_ring_visual(g,appearance,&image,0,0,128);}
                for(int y=0;y<128;++y)for(int x=0;x<128;++x){Gdiplus::Color pixel;canvas.GetPixel(x,y,&pixel);digests[layout]=digests[layout]*131+pixel.GetValue();}
                if(layout==2){Gdiplus::Color pixel;canvas.GetPixel(64,10,&pixel);assert(pixel.GetR()==0x12&&pixel.GetG()==0x34&&pixel.GetB()==0x56);}
            }
            for(std::size_t i=0;i<4;++i)for(std::size_t j=i+1;j<4;++j)assert(digests[i]!=digests[j]);
            {
                RingIcon caption;caption.text=L"Play / Pause";caption.font_family=L"Arial";caption.show_icon=false;caption.show_text=true;caption.text_color=0x00ff00;
                Gdiplus::Bitmap canvas(256,256,PixelFormat32bppARGB);{Gdiplus::Graphics g(&canvas);g.Clear(Gdiplus::Color(255,0,0,0));draw_ring_visual(g,caption,nullptr,0,0,256,&fonts);}
                int top=256,bottom=-1;for(int y=0;y<256;++y)for(int x=0;x<256;++x){Gdiplus::Color pixel;canvas.GetPixel(x,y,&pixel);if(pixel.GetG()>64){top=std::min(top,y);bottom=std::max(bottom,y);}}
                assert(bottom>=top&&bottom-top<40&&bottom<245);
            }
            Gdiplus::Bitmap white_mask(16,16,PixelFormat32bppARGB);{Gdiplus::Graphics g(&white_mask);g.Clear(Gdiplus::Color(255,255,255,255));}
            for(const bool mask:{false,true}){RingIcon tinted;tinted.glyph=mask?L"original-svg":L"keyboard";tinted.color=0x00ff00;Gdiplus::Bitmap canvas(64,64,PixelFormat32bppARGB);{Gdiplus::Graphics g(&canvas);g.Clear(Gdiplus::Color(255,0,0,0));draw_ring_visual(g,tinted,&white_mask,0,0,64);}
                Gdiplus::Color pixel;canvas.GetPixel(32,32,&pixel);assert(pixel.GetG()>240);assert(mask?(pixel.GetR()<5&&pixel.GetB()<5):(pixel.GetR()>240&&pixel.GetB()>240));}
        }
        Gdiplus::GdiplusShutdown(token);
        fs::remove(imported_path);fs::remove(directory/L"imported");fs::remove(directory);
    }
    std::puts("PASS: embedded image survives source deletion, GDI+ load, full background/four layouts, subset leaves unselected slots, persisted font family and drag transform.");
    std::puts("PASS: captured application profile, adaptive off/global fallback, named profiles, stale active profile, icon payload and metadata roundtrip.");
    std::puts("PASS: sparse slots keep positions; empty folder back/cancel; empty root; malformed markers rejected; legacy None unchanged.");
    std::puts("PASS: nested folders, multi-level Back, empty nested folders, cancel, shared subfolders; cycles and dangling nested targets rejected.");
    return 0;
}
