#pragma once
#include <array>
#include <charconv>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace loo_backup {
inline constexpr std::array<std::string_view,6> names{"dpi","smartshift","ratchet","hires","invert","thumb-invert"};
inline constexpr std::string_view marker="# Logi Options Open backup v1";
struct Configuration {
    std::string mappings;
    std::array<std::optional<unsigned>,6> settings{};
};
inline void validate(unsigned index,unsigned value){
    if((index==0&&(value<1||value>65535))||(index==1&&value>50)||(index>1&&value>1))
        throw std::runtime_error("Invalid backup device setting");
}
inline Configuration decode(std::string_view bytes){
    if(bytes.size()>131072||bytes.find('\0')!=bytes.npos)throw std::runtime_error("Invalid or oversized configuration backup");
    if(bytes.starts_with("\xef\xbb\xbf"))bytes.remove_prefix(3);
    Configuration result;
    if(bytes.starts_with("# Logi Options Open backup ")){
        const auto line=[&](){
            const auto end=bytes.find('\n');auto value=bytes.substr(0,end);
            bytes=end==bytes.npos?std::string_view{}:bytes.substr(end+1);
            if(value.ends_with('\r'))value.remove_suffix(1);
            return value;
        };
        if(line()!=marker)throw std::runtime_error("Unsupported configuration backup version");
        bool ended=false;
        while(!bytes.empty()){
            auto value=line();
            if(value=="# End backup settings"){ended=true;break;}
            if(!value.starts_with("# setting."))throw std::runtime_error("Invalid backup settings header");
            value.remove_prefix(10);const auto equal=value.find('=');
            if(equal==value.npos)throw std::runtime_error("Invalid backup setting");
            unsigned index=0;while(index<names.size()&&names[index]!=value.substr(0,equal))++index;
            if(index==names.size()||result.settings[index])throw std::runtime_error("Unknown or duplicate backup setting");
            const auto text=value.substr(equal+1);unsigned number{};
            const auto parsed=std::from_chars(text.data(),text.data()+text.size(),number);
            if(text.empty()||parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size())throw std::runtime_error("Invalid backup setting number");
            validate(index,number);result.settings[index]=number;
        }
        if(!ended)throw std::runtime_error("Incomplete configuration backup header");
    }
    if(bytes.empty()||bytes.size()>65535)throw std::runtime_error("Mappings must contain 1..65535 bytes");
    result.mappings=bytes;return result;
}
inline std::string encode(const Configuration& config){
    // Strip an earlier backup header rather than accumulating nested metadata.
    const auto mappings=decode(config.mappings).mappings;
    std::string output(marker);output+='\n';
    for(unsigned index=0;index<names.size();++index)if(config.settings[index]){
        validate(index,*config.settings[index]);
        output+="# setting.";output+=names[index];output+='=';
        output+=std::to_string(*config.settings[index]);output+='\n';
    }
    output+="# End backup settings\n";output+=mappings;return output;
}
}
