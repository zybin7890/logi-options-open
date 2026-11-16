#pragma once

#include "ring.hpp"

#include <array>
#include <map>
#include <sstream>
#include <string_view>
#include <vector>
#include <cstdlib>

namespace ring_metadata {

inline std::wstring_view default_glyph(std::wstring_view action) {
    if(action.empty())return L"add";
    if(action.starts_with(L"Folder(")||action==L"@folder"||action==L"Win+E")return L"folder";
    if(action==L"PlayPause")return L"play";
    if(action==L"MediaStop")return L"stop";
    if(action==L"Mute")return L"mute";
    if(action==L"VolumeUp"||action==L"VolumeDown")return L"volume";
    if(action==L"NextTrack")return L"next";
    if(action==L"PreviousTrack")return L"previous";
    if(action==L"Win+Shift+S"||action==L"PrintScreen")return L"capture";
    if(action==L"Win+Tab")return L"flow";
    if(action==L"Win+D")return L"desktop";
    if(action==L"Lock")return L"lock";
    if(action.starts_with(L"Open("))return L"note";
    if(action.starts_with(L"Mouse")||action.starts_with(L"Scroll")||action.starts_with(L"AdvancedClick("))return L"buttons";
    if(action.starts_with(L"DpiToggle("))return L"pointer";
    return L"keyboard";
}

struct Settings {
    struct Profile {std::wstring application,name,section;bool operator==(const Profile&) const=default;};
    bool adaptive{true};
    std::map<std::wstring,std::array<RingIcon,8>> icons;
    std::vector<Profile> profiles;
    std::map<std::wstring,std::wstring> active;
};

inline std::wstring hex(std::wstring_view text) {
    constexpr std::wstring_view digits=L"0123456789ABCDEF";
    std::wstring result;
    for(const auto unit:text) for(int shift=12;shift>=0;shift-=4) result+=digits[(unit>>shift)&15];
    return result.empty()?L"-":result;
}
inline std::optional<std::wstring> unhex(std::wstring_view value) {
    if(value==L"-")return std::wstring{};
    if(value.empty()||value.size()%4)return {};
    std::wstring result;
    for(std::size_t at=0;at<value.size();at+=4) {
        unsigned unit{};
        for(const auto ch:value.substr(at,4)) {
            const auto digit=ch>=L'0'&&ch<=L'9'?ch-L'0':ch>=L'A'&&ch<=L'F'?ch-L'A'+10:
                ch>=L'a'&&ch<=L'f'?ch-L'a'+10:-1;
            if(digit<0)return {};
            unit=(unit<<4)|digit;
        }
        if(!unit||unit==L'\r'||unit==L'\n')return {};
        result+=static_cast<wchar_t>(unit);
    }
    return result;
}
inline bool metadata_line(std::wstring_view line) {
    return line.starts_with(L"# ring-ui.adaptive=")||line.starts_with(L"# ring-ui.icon=")||
        line.starts_with(L"# ring-ui.profile=")||line.starts_with(L"# ring-ui.active=");
}
inline void consume(std::wstring_view line,Settings& settings) {
    constexpr std::wstring_view adaptive=L"# ring-ui.adaptive=",icon=L"# ring-ui.icon=";
    if(line.starts_with(adaptive)) {
        const auto value=line.substr(adaptive.size());
        if(value==L"0"||value==L"1")settings.adaptive=value==L"1";
    } else if(line.starts_with(icon)) {
        std::wistringstream input(std::wstring{line.substr(icon.size())});
        std::wstring section,glyph,image,background,extra;unsigned slot{},color{};
        if(!(input>>section>>slot>>glyph>>std::hex>>color>>background>>image)||
            slot<1||slot>8||color>0xffffff)return;
        const auto decoded_section=unhex(section),decoded_glyph=unhex(glyph),decoded_image=unhex(image);
        if(!decoded_section||decoded_section->empty()||!decoded_glyph||!decoded_image)return;
        std::optional<unsigned> fill;
        if(background!=L"-") {
            std::wistringstream value(background);unsigned rgb{};
            if(!(value>>std::hex>>rgb)||value>>extra||rgb>0xffffff)return;
            fill=rgb;
        }
        RingIcon result{*decoded_glyph,*decoded_image,color,fill};
        if(input>>extra) {
            const auto text=unhex(extra);unsigned flags{};
            if(!text||!(input>>result.text_color>>std::dec>>result.layout>>result.font_size>>result.scale>>
                result.offset_x>>result.offset_y>>flags)||result.text_color>0xffffff||
                result.layout<0||result.layout>3||result.font_size<8||result.font_size>96||
                result.scale<25||result.scale>200||std::abs(result.offset_x)>100||std::abs(result.offset_y)>100||flags>7)return;
            result.text=*text;result.show_icon=flags&1;result.show_text=flags&2;result.bold=flags&4;
            if(input>>extra){const auto family=unhex(extra);if(!family||family->empty()||input>>extra)return;result.font_family=*family;}
        }
        settings.icons[*decoded_section][slot-1]=std::move(result);
    } else if(line.starts_with(L"# ring-ui.profile=")||line.starts_with(L"# ring-ui.active=")) {
        const bool profile=line.starts_with(L"# ring-ui.profile=");
        std::wistringstream input(std::wstring{line.substr(profile?18:17)});
        std::wstring app,name,section,extra;
        if(!(input>>app>>section)||(profile&&!(input>>name))||input>>extra)return;
        const auto decoded_app=unhex(app),decoded_section=unhex(section),decoded_name=unhex(name);
        if(!decoded_app||!decoded_section)return;
        if(!decoded_section->starts_with(L"ring/")&&(!profile||(*decoded_section!=L"ring"&&!decoded_section->starts_with(L"ring:"))))return;
        if(profile) {if(decoded_name&&!decoded_name->empty())settings.profiles.push_back({*decoded_app,*decoded_name,*decoded_section});}
        else settings.active[*decoded_app]=*decoded_section;
    }
}
inline Settings read(std::wstring_view source) {
    Settings result;std::wistringstream input{std::wstring(source)};std::wstring line;
    while(std::getline(input,line)) {if(!line.empty()&&line.back()==L'\r')line.pop_back();consume(line,result);}
    return result;
}
inline std::wstring write(std::wstring_view source,const Settings& settings) {
    std::wostringstream output;output<<L"# ring-ui.adaptive="<<(settings.adaptive?1:0)<<L'\n';
    for(const auto& [section,icons]:settings.icons) for(std::size_t i=0;i<icons.size();++i) {
        const auto& icon=icons[i];
        if(icon==RingIcon{})continue;
        output<<L"# ring-ui.icon="<<hex(section)<<L' '<<std::dec<<i+1<<L' '<<hex(icon.glyph)<<L' '
            <<std::hex<<icon.color<<L' ';
        if(icon.background)output<<*icon.background;else output<<L'-';
        output<<L' '<<hex(icon.image)<<L' '<<hex(icon.text)<<L' '<<icon.text_color<<L' '<<std::dec
            <<icon.layout<<L' '<<icon.font_size<<L' '<<icon.scale<<L' '<<icon.offset_x<<L' '<<icon.offset_y<<L' '
            <<(unsigned(icon.show_icon)|unsigned(icon.show_text)*2|unsigned(icon.bold)*4)<<L' '<<hex(icon.font_family)<<L'\n';
    }
    for(const auto& profile:settings.profiles)output<<L"# ring-ui.profile="<<hex(profile.application)<<L' '<<hex(profile.section)<<L' '<<hex(profile.name)<<L'\n';
    for(const auto& [app,section]:settings.active)output<<L"# ring-ui.active="<<hex(app)<<L' '<<hex(section)<<L'\n';
    std::wistringstream input{std::wstring(source)};std::wstring line;
    while(std::getline(input,line)) {
        auto plain=std::wstring_view(line);if(plain.ends_with(L'\r'))plain.remove_suffix(1);
        if(!metadata_line(plain))output<<line<<L'\n';
    }
    return output.str();
}
inline RingIcon icon(const Settings& settings,std::wstring_view section,std::size_t index) {
    const auto found=settings.icons.find(std::wstring(section));
    return found!=settings.icons.end()&&index<8?found->second[index]:RingIcon{};
}
inline void apply_icon(Settings&settings,const std::wstring&section,unsigned slots,const RingIcon&icon) {
    for(unsigned index=0;index<8;++index)if(slots&(1u<<index))settings.icons[section][index]=icon;
}
inline std::wstring root(std::wstring_view application) {
    return application.empty()||application==L"global"?L"ring":L"ring:"+std::wstring(application);
}

} // namespace ring_metadata
