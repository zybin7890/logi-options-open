#include "../smooth_scroll.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(0)
int main(){try{
    using namespace smooth_scroll;
    Filter filter;filter.feed(120,12);
    CHECK(std::abs(filter.gain-1)<1e-12);
    CHECK(std::abs(filter.tick()-72)<1e-9);
    CHECK(std::abs(filter.tick()-9.6)<1e-9);
    Axis axis;CHECK(axis.feed(120,100,true)==0);CHECK(axis.tick(109)==0);
    CHECK(axis.tick(110)==72);int sum=72;unsigned outputs=1;
    for(unsigned t=120;t<20000&&axis.active();t+=10){auto n=axis.tick(t);sum+=n;outputs+=n!=0;}
    CHECK(!axis.active());CHECK(sum==120);CHECK(outputs>2);CHECK(axis.tick(30000)==0);
    for(unsigned multiplier:{7,8,120}) {
        axis.reset();int total{};
        for(unsigned i=0;i<multiplier;++i){total+=axis.feed(120./multiplier,100+i*10,true);total+=axis.tick(110+i*10);}
        for(unsigned t=100+multiplier*10;t<30000&&axis.active();t+=10)total+=axis.tick(t);
        CHECK(total==120);CHECK(!axis.active());
    }
    axis.reset();CHECK(axis.feed(119,100,false)==0);CHECK(axis.feed(1,200,false)==120);
    CHECK(axis.feed(-130,300,false)==-120);CHECK(axis.feed(10,400,false)==0);CHECK(!axis.active());
    axis.reset();axis.feed(120,100,true);axis.feed(-120,100,true);CHECK(axis.tick(110)==0);CHECK(!axis.active());
    axis.feed(120,200,true);axis.reset();CHECK(axis.tick(210)==0);CHECK(!axis.active());
    const std::array<std::uint8_t,3> high{0x13,0xff,0xf8},low{0x01,0,1},bad{0x81,0,1};
    const auto h=decode(high,8,false),i=decode(high,8,true),l=decode(low,8,false);
    CHECK(h&&h->units==-120&&h->periods==3&&h->high_resolution);CHECK(i&&i->units==120);
    CHECK(l&&l->units==120&&!l->high_resolution);CHECK(!decode(bad,8,false));CHECK(!decode(low,0,false));
    CHECK(!decode(std::span(low).first(2),8,false));
    std::cout<<"PASS smooth filter, timing, signed sub-count conservation, idle, cancellation and 0x2121 decoding\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
