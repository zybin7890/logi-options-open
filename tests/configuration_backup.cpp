#include "../configuration_backup.hpp"
#include <iostream>
using namespace loo_backup;
void require(bool value){if(!value)throw std::runtime_error("Backup round-trip assertion failed");}
template<class F>void reject(F action){bool failed=false;try{action();}catch(const std::exception&){failed=true;}require(failed);}
int main(){try{
    const std::string mappings="; UTF-8 comment: 鼠标\r\n[global]\r\n0053=MouseBack\r\n[firefox]\r\n0053=Ctrl+Tab\r\n[hotkeys]\r\nCtrl+E=Wait(20);Text(你好)\r\n[ring]\r\nMedia=Folder(Media)\r\n[ring/Media]\r\nPause=PlayPause\r\n";
    Configuration source{mappings,{5000,12,1,0,0,1}};
    const auto encoded=encode(source);const auto restored=decode(encoded);
    require(restored.mappings==mappings&&restored.settings==source.settings);
    require(encode({encoded,source.settings})==encoded);
    require(decode(mappings).mappings==mappings);
    require(decode("\xef\xbb\xbf"+encoded).settings==source.settings);
    reject([&]{decode("# Logi Options Open backup v2\n"+mappings);});
    reject([&]{decode(std::string(marker)+"\n# setting.dpi=5000\n");});
    reject([&]{decode(std::string(marker)+"\n# setting.dpi=0\n# End backup settings\n"+mappings);});
    reject([&]{decode(std::string(marker)+"\n# setting.hires=2\n# End backup settings\n"+mappings);});
    reject([&]{decode(std::string(marker)+"\n# setting.hires=0\n# setting.hires=0\n# End backup settings\n"+mappings);});
    reject([&]{decode(std::string(marker)+"\n# setting.unknown=1\n# End backup settings\n"+mappings);});
    reject([&]{decode(std::string(65536,'a'));});
    reject([&]{decode(std::string("a\0b",3));});
    std::cout<<"PASS: full mapping/settings UTF-8 round trip, legacy INI, version and malformed-header rejection.\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
