#pragma once
#include <linux/input-event-codes.h>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace loo_linux {
enum class Direction {tap,left,right,up,down};
enum class Op {keys,wait,wheel_mode,scroll_x,scroll_y,open};
struct Action {Op operation{Op::keys};std::vector<unsigned> keys;int value{};std::string text;};
using Binding=std::pair<unsigned,Direction>;
constexpr unsigned thumb=0x10000;
struct Config {std::map<Binding,std::vector<Action>> bindings;unsigned speed{100};};
inline std::string_view trim(std::string_view text) {
    const auto first=text.find_first_not_of(" \r\t\n");
    return first==text.npos?std::string_view{}:text.substr(first,text.find_last_not_of(" \r\t\n")-first+1);
}
inline unsigned number(std::string_view text,unsigned base=10) {
    unsigned result{};const auto parsed=std::from_chars(text.data(),text.data()+text.size(),result,base);
    if(text.empty() || parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size()) throw std::runtime_error("Invalid number");
    return result;
}
inline unsigned key(std::string_view name) {
    static const std::map<std::string_view,unsigned> names={
        {"Ctrl",KEY_LEFTCTRL},{"Alt",KEY_LEFTALT},{"Shift",KEY_LEFTSHIFT},{"Super",KEY_LEFTMETA},{"Win",KEY_LEFTMETA},
        {"Left",KEY_LEFT},{"Right",KEY_RIGHT},{"Up",KEY_UP},{"Down",KEY_DOWN},{"Tab",KEY_TAB},
        {"Enter",KEY_ENTER},{"Esc",KEY_ESC},{"Space",KEY_SPACE},{"Backspace",KEY_BACKSPACE},
        {"Delete",KEY_DELETE},{"Home",KEY_HOME},{"End",KEY_END},{"PageUp",KEY_PAGEUP},{"PageDown",KEY_PAGEDOWN},
        {"PrintScreen",KEY_SYSRQ},{"VolumeUp",KEY_VOLUMEUP},{"VolumeDown",KEY_VOLUMEDOWN},{"Mute",KEY_MUTE},
        {"PlayPause",KEY_PLAYPAUSE},{"NextTrack",KEY_NEXTSONG},{"PreviousTrack",KEY_PREVIOUSSONG},
        {"MouseMiddle",BTN_MIDDLE},{"MouseBack",BTN_SIDE},{"MouseForward",BTN_EXTRA}};
    if(auto found=names.find(name);found!=names.end()) return found->second;
    constexpr unsigned letters[]={KEY_A,KEY_B,KEY_C,KEY_D,KEY_E,KEY_F,KEY_G,KEY_H,KEY_I,KEY_J,KEY_K,KEY_L,KEY_M,
                                  KEY_N,KEY_O,KEY_P,KEY_Q,KEY_R,KEY_S,KEY_T,KEY_U,KEY_V,KEY_W,KEY_X,KEY_Y,KEY_Z};
    if(name.size()==1 && name[0]>='A' && name[0]<='Z') return letters[name[0]-'A'];
    if(name.size()==1 && name[0]>='0' && name[0]<='9') return name[0]=='0'?KEY_0:KEY_1+(name[0]-'1');
    if(name.starts_with('F')) {const auto n=number(name.substr(1));if(n>=1 && n<=10) return KEY_F1+n-1;if(n==11)return KEY_F11;if(n==12)return KEY_F12;}
    throw std::runtime_error("Unsupported Linux key: "+std::string(name));
}
inline std::vector<Action> actions(std::string_view value) {
    std::vector<Action> result;
    while(!value.empty()) {
        const auto end=value.find(';');auto text=trim(value.substr(0,end));
        if(text.empty()) throw std::runtime_error("Empty action");
        Action action;
        if(text=="None") {}
        else if(text=="WheelMode") action.operation=Op::wheel_mode;
        else if(text=="ScrollLeft" || text=="ScrollRight") {action.operation=Op::scroll_x;action.value=text=="ScrollLeft"?-1:1;}
        else if(text=="ScrollUp" || text=="ScrollDown") {action.operation=Op::scroll_y;action.value=text=="ScrollUp"?1:-1;}
        else if(text.starts_with("Wait(") && text.ends_with(')')) {
            action.operation=Op::wait;action.value=static_cast<int>(number(text.substr(5,text.size()-6)));
            if(action.value<0 || action.value>60000) throw std::runtime_error("Wait must be 0..60000 ms");
        } else if(text.starts_with("Open(") && text.ends_with(')')) {
            action.operation=Op::open;action.text=std::string(text.substr(5,text.size()-6));
            if(action.text.empty() || action.text[0]=='-' || action.text.find('\0')!=std::string::npos)
                throw std::runtime_error("Open requires a file or URL");
        } else {
            while(!text.empty()) {
                const auto plus=text.find('+');const auto code=key(trim(text.substr(0,plus)));
                if(std::ranges::find(action.keys,code)!=action.keys.end()) throw std::runtime_error("Duplicate key in chord");
                action.keys.push_back(code);
                if(plus==text.npos) break;
                text=trim(text.substr(plus+1));if(text.empty()) throw std::runtime_error("Missing chord key");
            }
            if(action.keys.size()>8) throw std::runtime_error("Chord has too many keys");
        }
        result.push_back(std::move(action));if(result.size()>64) throw std::runtime_error("Too many actions");
        if(end==value.npos) break;
        value=trim(value.substr(end+1));if(value.empty()) throw std::runtime_error("Trailing action separator");
    }
    if(result.empty()) throw std::runtime_error("Empty action sequence");
    return result;
}
inline Binding binding(std::string_view text) {
    const auto dot=text.find('.');const auto id=text.substr(0,dot);
    unsigned code=id=="thumb"?thumb:number(id,16);
    if(code!=thumb && code>65535) throw std::runtime_error("Invalid button control id");
    Direction direction=Direction::tap;
    if(dot!=text.npos) {
        const auto suffix=text.substr(dot+1);
        if(suffix=="left") direction=Direction::left;
        else if(suffix=="right") direction=Direction::right;
        else if(suffix=="up") direction=Direction::up;
        else if(suffix=="down") direction=Direction::down;
        else throw std::runtime_error("Unknown gesture direction");
    }
    if(code==thumb && direction!=Direction::left && direction!=Direction::right)
        throw std::runtime_error("Thumb wheel requires left or right");
    return {code,direction};
}
inline Config parse(std::string_view content) {
    Config config;bool global=false;unsigned line_number=0;
    while(!content.empty()) {
        ++line_number;const auto end=content.find('\n');auto line=trim(content.substr(0,end));
        content=end==content.npos?std::string_view{}:content.substr(end+1);
        if(line.empty() || line[0]=='#' || line[0]==';') continue;
        try {
            if(line.front()=='[') {
                if(line!="[global]") throw std::runtime_error("Linux supports [global] mappings only; app profiles/hotkeys/Ring are not implemented");
                global=true;continue;
            }
            if(!global) throw std::runtime_error("Expected [global] section");
            const auto equals=line.find('=');if(equals==line.npos) throw std::runtime_error("Expected key = action");
            const auto name=trim(line.substr(0,equals));const auto value=trim(line.substr(equals+1));
            if(name=="thumb.speed") {config.speed=number(value);if(config.speed<10 || config.speed>200)throw std::runtime_error("Thumb speed must be 10..200");continue;}
            const auto id=binding(name);if(config.bindings.contains(id)) throw std::runtime_error("Duplicate mapping");
            config.bindings.emplace(id,actions(value));
        } catch(const std::exception& error) {throw std::runtime_error("Line "+std::to_string(line_number)+": "+error.what());}
    }
    if(config.bindings.empty()) throw std::runtime_error("No mappings configured");
    for(const auto& [id,sequence]:config.bindings) {
        if(id.second!=Direction::tap && id.first!=thumb && !config.bindings.contains({id.first,Direction::tap}))
            throw std::runtime_error("A gesture control requires a tap binding (None is allowed)");
    }
    return config;
}
inline Config load(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("Cannot open mappings: "+path.string());
    std::string text((std::istreambuf_iterator<char>(file)),{});
    if(text.size()>65535) throw std::runtime_error("Mappings exceed 64 KiB");
    if(text.starts_with("\xef\xbb\xbf"))text.erase(0,3);
    return parse(text);
}
inline std::map<unsigned,bool> layout(const Config& config) {
    std::map<unsigned,bool> result;
    for(const auto& [id,sequence]:config.bindings) result[id.first]|=id.first!=thumb && id.second!=Direction::tap;
    return result;
}
inline Direction gesture(int x,int y,int threshold) {
    if(std::max(std::abs(x),std::abs(y))<threshold) return Direction::tap;
    return std::abs(x)>=std::abs(y)?(x<0?Direction::left:Direction::right):(y<0?Direction::up:Direction::down);
}
struct WheelAccumulator {
    std::int64_t remainder{};
    int steps(int rotation,unsigned native,unsigned diverted,int direction,unsigned speed,bool begin=false) {
        if(begin) remainder=0;
        if(!diverted || !native)return 0;
        remainder+=std::int64_t(rotation)*native*direction*speed;
        const auto divisor=std::int64_t(diverted)*100;
        const auto whole=remainder/divisor;remainder%=divisor;
        return static_cast<int>(std::clamp<std::int64_t>(whole,-32,32));
    }
};
} // namespace loo_linux
