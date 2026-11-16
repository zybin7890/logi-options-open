#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace native_smartshift {
// Verified in Options+ 2110 and 2111 SettingsManager::write. Rounding the
// negative term before adding 50 matters at the 25% and 75% boundaries.
inline unsigned threshold(unsigned percent) {
    return static_cast<unsigned>(50+std::lround(-46.0*std::min(percent,100u)/100.0));
}
inline unsigned percent(unsigned threshold) {
    return static_cast<unsigned>((50-static_cast<int>(std::clamp(threshold,4u,50u)))*100.0/46.0);
}
inline constexpr std::wstring_view marker=L"# UI smartshift.sensitivity=";
inline std::optional<unsigned> read(std::wstring_view source) {
    while(!source.empty()) {
        const auto end=source.find(L'\n');auto line=source.substr(0,end);
        if(line.ends_with(L'\r'))line.remove_suffix(1);
        if(line.starts_with(marker)) {
            line.remove_prefix(marker.size());
            if(line.empty() || line.size()>3)return {};
            unsigned value{};
            for(const auto c:line) {if(c<L'0'||c>L'9')return {};value=value*10+c-L'0';}
            return value<=100?std::optional{value}:std::nullopt;
        }
        source=end==source.npos?std::wstring_view{}:source.substr(end+1);
    }
    return {};
}
inline std::wstring write(std::wstring_view source,unsigned percent) {
    std::wstring result=std::wstring(marker)+std::to_wstring(std::min(percent,100u))+L"\n";
    while(!source.empty()) {
        const auto end=source.find(L'\n');auto line=source.substr(0,end);
        if(!line.starts_with(marker)) {result+=line;if(end!=source.npos)result+=L'\n';}
        source=end==source.npos?std::wstring_view{}:source.substr(end+1);
    }
    return result;
}
inline unsigned display(unsigned raw,std::optional<unsigned> saved) {
    if(saved && (!raw || threshold(*saved)==raw))return *saved;
    return raw?percent(raw):83;
}
}
