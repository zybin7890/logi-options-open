#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace native_dpi {
// MX Master 3S's Options+ model uses a logarithmic curve about the midpoint.
inline unsigned from_percent(unsigned percent, bool extended) {
    const double middle=extended?2000.0:1000.0;
    const double bound=percent<50?200.0:(extended?8000.0:4000.0);
    const double distance=std::abs(static_cast<double>(std::min(percent,100u))/50.0-1.0);
    const auto raw=std::lround(std::pow(middle,1.0-distance)*std::pow(bound,distance));
    return static_cast<unsigned>(std::clamp(200L+std::lround((raw-200)/50.0)*50L,
                                           200L,extended?8000L:4000L));
}
inline unsigned to_percent(unsigned dpi, bool extended) {
    const double middle=extended?2000.0:1000.0;
    const double value=std::clamp(static_cast<double>(dpi),200.0,extended?8000.0:4000.0);
    if (value==middle) return 50;
    const double bound=value<middle?200.0:(extended?8000.0:4000.0);
    const double sign=value<middle?-1.0:1.0;
    return static_cast<unsigned>(std::clamp(std::lround((sign*std::log(value/middle)/std::log(bound/middle)+1.0)*50.0),0L,100L));
}
inline unsigned supported(unsigned wanted,std::span<const unsigned> choices) {
    if(choices.empty()) return wanted;
    return *std::ranges::min_element(choices,{},[wanted](unsigned value){
        return value>wanted?value-wanted:wanted-value;
    });
}
inline constexpr std::wstring_view marker=L"# UI dpi.high-resolution=";
inline std::optional<bool> read_mode(std::wstring_view source) {
    while(!source.empty()) {
        const auto end=source.find(L'\n');
        auto line=source.substr(0,end);
        if(line.ends_with(L'\r'))line.remove_suffix(1);
        if(line==std::wstring(marker)+L"1")return true;
        if(line==std::wstring(marker)+L"0")return false;
        source=end==source.npos?std::wstring_view{}:source.substr(end+1);
    }
    return {};
}
inline std::wstring write_mode(std::wstring_view source,bool extended) {
    std::wstring result=std::wstring(marker)+(extended?L"1\n":L"0\n");
    while(!source.empty()) {
        const auto end=source.find(L'\n');
        auto line=source.substr(0,end);
        if(!line.starts_with(marker)) {result+=line;if(end!=source.npos)result+=L'\n';}
        source=end==source.npos?std::wstring_view{}:source.substr(end+1);
    }
    return result;
}
}
