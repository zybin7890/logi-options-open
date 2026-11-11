#pragma once
#include <linux/input-event-codes.h>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace loo_linux {
enum class Direction {tap,left,right,up,down};
enum class Op {keys,wait,wheel_mode,scroll_x,scroll_y,open,text,ring,folder};
struct Action {Op operation{Op::keys};std::vector<unsigned> keys;int value{};std::string text;};
using Binding=std::pair<unsigned,Direction>;
constexpr unsigned thumb=0x10000;
struct Profile {std::map<Binding,std::vector<Action>> bindings;std::optional<unsigned> speed;};
struct Hotkey {std::string name;std::vector<unsigned> keys;std::vector<Action> actions;};
struct RingItem {std::string label;std::vector<Action> actions;};
struct Config {
    std::map<Binding,std::vector<Action>> bindings;unsigned speed{100};
    std::map<std::string,Profile> profiles;
    std::vector<Hotkey> hotkeys;
    std::vector<RingItem> ring;
    std::map<std::string,std::vector<RingItem>> folders;
};
inline std::string normalized(std::string_view name) {
    std::string result(name);
    for(auto& c:result)if(c>='A' && c<='Z')c=static_cast<char>(c-'A'+'a');
    return result;
}
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
        auto end=value.npos;unsigned depth=0;
        for(std::size_t i=0;i<value.size();++i) {
            if(value[i]=='(')++depth;
            else if(value[i]==')'){if(!depth)throw std::runtime_error("Unbalanced action parentheses");--depth;}
            else if(value[i]==';' && !depth){end=i;break;}
        }
        if(depth)throw std::runtime_error("Unbalanced action parentheses");
        auto text=trim(value.substr(0,end));
        if(text.empty()) throw std::runtime_error("Empty action");
        Action action;
        if(text=="None") {}
        else if(text=="WheelMode") action.operation=Op::wheel_mode;
        else if(text=="Ring") action.operation=Op::ring;
        else if(text.starts_with("Folder(") && text.ends_with(')')) {
            action.operation=Op::folder;action.text=std::string(trim(text.substr(7,text.size()-8)));
            if(action.text.empty())throw std::runtime_error("Folder requires a name");
        }
        else if(text=="ScrollLeft" || text=="ScrollRight") {action.operation=Op::scroll_x;action.value=text=="ScrollLeft"?-1:1;}
        else if(text=="ScrollUp" || text=="ScrollDown") {action.operation=Op::scroll_y;action.value=text=="ScrollUp"?1:-1;}
        else if(text.starts_with("Wait(") && text.ends_with(')')) {
            action.operation=Op::wait;action.value=static_cast<int>(number(text.substr(5,text.size()-6)));
            if(action.value<0 || action.value>60000) throw std::runtime_error("Wait must be 0..60000 ms");
        } else if((text.starts_with("Open(") || text.starts_with("Text(")) && text.ends_with(')')) {
            action.operation=text.starts_with("Text(")?Op::text:Op::open;
            action.text=std::string(text.substr(5,text.size()-6));
            if(action.text.empty() || action.text.size()>8192 || action.text.find('\0')!=std::string::npos ||
               (action.operation==Op::open && action.text[0]=='-'))
                throw std::runtime_error("Text/Open requires a non-empty value (max 8192 UTF-8 bytes)");
        } else if(text.starts_with("TextHex(") && text.ends_with(')')) {
            action.operation=Op::text;auto encoded=text.substr(8,text.size()-9);
            if(encoded.empty() || encoded.size()%4 || encoded.size()>4000)throw std::runtime_error("Invalid TextHex UTF-16");
            for(std::size_t offset=0;offset<encoded.size();offset+=4) {
                auto code=number(encoded.substr(offset,4),16);
                if(code>=0xd800 && code<=0xdbff) {
                    if(offset+8>encoded.size())throw std::runtime_error("Unpaired TextHex surrogate");
                    const auto low=number(encoded.substr(offset+4,4),16);offset+=4;
                    if(low<0xdc00 || low>0xdfff)throw std::runtime_error("Unpaired TextHex surrogate");
                    code=0x10000+((code-0xd800)<<10)+(low-0xdc00);
                } else if(code>=0xdc00 && code<=0xdfff)throw std::runtime_error("Unpaired TextHex surrogate");
                if(!code)throw std::runtime_error("TextHex cannot contain NUL");
                if(code<0x80)action.text+=static_cast<char>(code);
                else {
                    if(code<0x800)action.text+=static_cast<char>(0xc0|(code>>6));
                    else {
                        if(code<0x10000)action.text+=static_cast<char>(0xe0|(code>>12));
                        else {action.text+=static_cast<char>(0xf0|(code>>18));action.text+=static_cast<char>(0x80|((code>>12)&63));}
                        action.text+=static_cast<char>(0x80|((code>>6)&63));
                    }
                    action.text+=static_cast<char>(0x80|(code&63));
                }
            }
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
    if(result.size()>1 && std::ranges::any_of(result,[](const Action& a){return a.operation==Op::ring || a.operation==Op::folder;}))
        throw std::runtime_error("Ring/Folder must be the only action");
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
    Config config;std::string section;unsigned line_number=0;std::set<std::vector<unsigned>> chords;
    while(!content.empty()) {
        ++line_number;const auto end=content.find('\n');auto line=trim(content.substr(0,end));
        content=end==content.npos?std::string_view{}:content.substr(end+1);
        if(line.empty() || line[0]=='#' || line[0]==';') continue;
        try {
            if(line.front()=='[') {
                if(!line.ends_with(']'))throw std::runtime_error("Invalid section");
                section=std::string(trim(line.substr(1,line.size()-2)));
                if(section.empty() || section.find_first_of("[]\r\n")!=section.npos)throw std::runtime_error("Invalid section name");
                if(section.starts_with("ring/")) {
                    if(section.size()==5)throw std::runtime_error("Ring folder name is empty");
                    config.folders.try_emplace(section.substr(5));
                } else {section=normalized(section);if(section!="global" && section!="hotkeys" && section!="ring")config.profiles.try_emplace(section);}
                continue;
            }
            if(section.empty()) throw std::runtime_error("Expected a section");
            const auto equals=line.find('=');if(equals==line.npos) throw std::runtime_error("Expected key = action");
            const auto name=trim(line.substr(0,equals));const auto value=trim(line.substr(equals+1));
            if(section=="hotkeys") {
                auto chord=actions(name);
                if(chord.size()!=1 || chord[0].operation!=Op::keys || chord[0].keys.empty())throw std::runtime_error("Invalid trigger chord");
                auto keys=chord[0].keys;std::ranges::sort(keys);
                if(!chords.insert(keys).second)throw std::runtime_error("Duplicate hotkey");
                unsigned normal=0;
                for(auto code:keys) {
                    if(code>=BTN_MISC)throw std::runtime_error("Global triggers require keyboard keys");
                    if(code!=KEY_LEFTCTRL && code!=KEY_LEFTALT && code!=KEY_LEFTSHIFT && code!=KEY_LEFTMETA)++normal;
                }
                if(normal!=1)throw std::runtime_error("Global triggers require exactly one non-modifier key");
                config.hotkeys.push_back({std::string(name),std::move(keys),actions(value)});continue;
            }
            if(section=="ring" || section.starts_with("ring/")) {
                auto& items=section=="ring"?config.ring:config.folders.at(section.substr(5));
                if(name.empty() || items.size()>=8)throw std::runtime_error("Ring needs labels and at most eight items per folder");
                if(std::ranges::any_of(items,[&](const RingItem& item){return item.label==name;}))throw std::runtime_error("Duplicate Ring label");
                items.push_back({std::string(name),actions(value)});continue;
            }
            if(name=="thumb.speed") {
                auto speed=number(value);if(speed<10 || speed>200)throw std::runtime_error("Thumb speed must be 10..200");
                if(section=="global")config.speed=speed;else config.profiles.at(section).speed=speed;continue;
            }
            auto& bindings=section=="global"?config.bindings:config.profiles.at(section).bindings;
            const auto id=binding(name);if(bindings.contains(id)) throw std::runtime_error("Duplicate mapping");
            bindings.emplace(id,actions(value));
        } catch(const std::exception& error) {throw std::runtime_error("Line "+std::to_string(line_number)+": "+error.what());}
    }
    if(config.bindings.empty() && config.profiles.empty() && config.hotkeys.empty() && config.ring.empty())throw std::runtime_error("No mappings configured");
    auto check=[&](const auto& bindings) {
        for(const auto& [id,sequence]:bindings)
            if(id.second!=Direction::tap && id.first!=thumb && !bindings.contains({id.first,Direction::tap}) && !config.bindings.contains({id.first,Direction::tap}))
                throw std::runtime_error("A gesture control requires a tap binding (None is allowed)");
    };
    check(config.bindings);for(const auto& [name,profile]:config.profiles)check(profile.bindings);
    for(const auto& [name,profile]:config.profiles)for(const auto& [id,sequence]:profile.bindings)
        if(!config.bindings.contains({id.first,Direction::tap}) && id.first!=thumb && id.first!=0x52 && id.first!=0x53 && id.first!=0x56 && id.first!=0xc4)
            throw std::runtime_error("App-only control requires a global tap fallback: "+name);
    auto references=[&](const std::vector<Action>& sequence) {
        for(const auto& action:sequence) {
            if(action.operation==Op::ring && config.ring.empty())throw std::runtime_error("Ring requires a nonempty [ring] section");
            if(action.operation==Op::folder && (!config.folders.contains(action.text) || config.folders.at(action.text).empty()))
                throw std::runtime_error("Folder requires a nonempty [ring/"+action.text+"] section");
        }
    };
    for(const auto& [id,sequence]:config.bindings)references(sequence);
    for(const auto& [name,profile]:config.profiles)for(const auto& [id,sequence]:profile.bindings)references(sequence);
    for(const auto& trigger:config.hotkeys)references(trigger.actions);
    for(const auto& item:config.ring)references(item.actions);
    for(const auto& [name,items]:config.folders)for(const auto& item:items)references(item.actions);
    // Folder navigation must form a DAG; cycles otherwise produce an endless
    // nested Ring path and cannot be left using the normal back action.
    std::map<std::string,unsigned> visited;
    auto visit=[&](auto&& self,const std::string& name)->void {
        if(visited[name]==1)throw std::runtime_error("Ring folder cycle: "+name);
        if(visited[name]==2)return;
        visited[name]=1;
        const auto& items=name.empty()?config.ring:config.folders.at(name);
        for(const auto& item:items)for(const auto& action:item.actions) {
            if(action.operation==Op::folder)self(self,action.text);
            if(action.operation==Op::ring)self(self,"");
        }
        visited[name]=2;
    };
    visit(visit,"");for(const auto& [name,items]:config.folders)visit(visit,name);
    return config;
}
inline const Profile* selected_profile(const Config& config,const std::vector<std::string>& identities) {
    for(const auto& identity:identities)if(auto it=config.profiles.find(normalized(identity));it!=config.profiles.end())return &it->second;
    return nullptr;
}
inline const std::vector<Action>* lookup(const Config& config,Binding id,const std::vector<std::string>& identities={}) {
    if(auto profile=selected_profile(config,identities))if(auto it=profile->bindings.find(id);it!=profile->bindings.end())return &it->second;
    if(auto it=config.bindings.find(id);it!=config.bindings.end())return &it->second;
    return nullptr;
}
inline unsigned speed_for(const Config& config,const std::vector<std::string>& identities={}) {
    if(auto profile=selected_profile(config,identities);profile && profile->speed)return *profile->speed;
    return config.speed;
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
    for(const auto& [name,profile]:config.profiles)
        for(const auto& [id,sequence]:profile.bindings)result[id.first]|=id.first!=thumb && id.second!=Direction::tap;
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
