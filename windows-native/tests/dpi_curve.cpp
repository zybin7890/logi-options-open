#include "../native_dpi.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

int main() {
    const auto require=[](bool condition){if(!condition)throw std::runtime_error("DPI curve mismatch");};
    using namespace native_dpi;
    for(bool extended:{false,true}) {
        require(from_percent(0,extended)==200);
        require(from_percent(50,extended)==(extended?2000u:1000u));
        require(from_percent(100,extended)==(extended?8000u:4000u));
        require(to_percent(200,extended)==0);
        require(to_percent(extended?8000:4000,extended)==100);
        unsigned previous=0;
        for(unsigned p=0;p<=100;++p) {
            const auto value=from_percent(p,extended);
            require(value>=previous && value%50==0);
            previous=value;
        }
    }
    // Observed in the original MX Master 3S panel, not a linear 8000-DPI scale.
    require(to_percent(5000,true)==83);
    require(from_percent(83,true)==5000);
    require(to_percent(1000,false)==50);
    require(to_percent(1000,true)==35);
    require(to_percent(4000,true)==75);
    require(from_percent(101,false)==4000);
    const std::array<unsigned,4> sparse{200,400,800,1600};
    require(supported(750,sparse)==800);
    const std::wstring original=L"# personal comment\r\n[global]\r\n0053 = MouseBack\r\n";
    require(!read_mode(original).has_value());
    const auto on=write_mode(original,true);
    require(read_mode(on)==true && on.ends_with(original));
    const auto off=write_mode(on,false);
    require(read_mode(off)==false && off.ends_with(original));
    require(write_mode(off,false)==off);
    std::cout<<"Original DPI curve and persistent range metadata passed\n";
}
