#include "../discovery_route.hpp"

#include <iostream>
#include <stdexcept>

int main() {
    const auto require = [](bool condition) {
        if (!condition) throw std::runtime_error("Discovery route hint mismatch");
    };
    using namespace logi::discovery;
    const logi::Interface first{.path=L"receiver-a", .receiver=true};
    const logi::Interface wanted{.path=L"receiver-b", .receiver=true};
    const logi::Interface direct{.path=L"direct-mouse", .receiver=false};
    const Hint hint{4, wanted.path};
    require(decode(encode(hint)) == hint);
    const Hint unicode{255, L"\\\\?\\hid#罗技设备"};
    require(decode(encode(unicode)) == unicode);
    for (const auto bad : {L"", L"4", L"4\n", L"0\nx", L"7\nx", L"254\nx", L"256\nx",
                           L"-1\nx", L"4x\nx", L"0004\nx", L"4\nx\ny", L"4\nx\r"})
        require(!decode(bad));
    require(!decode(std::wstring_view{L"4\nx\0y", 6}));

    std::vector<logi::Interface> interfaces{first, wanted, direct};
    prioritize(interfaces, hint, false);
    require(interfaces[0].path == wanted.path && interfaces[1].path == first.path &&
            interfaces[2].path == direct.path);
    require(slots(wanted, hint, false) == std::vector<unsigned>{4, 1, 2, 3, 5, 6});
    // After a stale preferred slot fails, the real connect loop visits all other
    // slots once, including a mouse newly paired in a lower-numbered slot.
    const auto attempts = slots(wanted, hint, false);
    unsigned tries{};
    for (const auto slot : attempts) { ++tries; if (slot == 2) break; }
    require(tries == 3);
    require(std::ranges::count(attempts, 4) == 1);
    require(slots(first, hint, false) == std::vector<unsigned>{1, 2, 3, 4, 5, 6});
    require(slots(direct, hint, false) == std::vector<unsigned>{255});
    require(slots(wanted, Hint{255,wanted.path}, false) == std::vector<unsigned>{1, 2, 3, 4, 5, 6});
    require(slots(direct, Hint{4,direct.path}, false) == std::vector<unsigned>{255});

    interfaces = {first, wanted, direct};
    prioritize(interfaces, hint, true);
    require(interfaces[0].path == first.path && interfaces[1].path == wanted.path);
    require(slots(wanted, hint, true) == std::vector<unsigned>{1, 2, 3, 4, 5, 6});
    const Hint absent{4,L"removed-receiver"};
    prioritize(interfaces, absent, false);
    require(interfaces[0].path == first.path && interfaces[1].path == wanted.path);
    require(slots(wanted, absent, false) == std::vector<unsigned>{1, 2, 3, 4, 5, 6});
    // Cold start, no hint, receiver is present but slots 1..3 never reply.
    // Model the identical device latency for the CLI scan and the owner scan.
    const std::vector<logi::Interface> one{wanted};
    auto cold_slots=targets(one,{},false);
    const auto latency=[](const std::vector<Attempt>& order)->DWORD {
        DWORD elapsed{};
        for(const auto attempt:order) {
            if(attempt.slot==4)return elapsed+16;
            elapsed+=attempt.ping_ms;
        }
        return INFINITE;
    };
    OwnerSearch cold;
    const auto cold_ms=latency(cold.round(cold_slots,false,0));
    require(cold_ms==556 && latency(cold_slots)==3766);
    // Stale hint (4), replacement in slot 2 needs 450 ms, so short probes
    // never work. Long-timeout coverage must still reach the replacement.
    OwnerSearch warm;
    const auto preferred=targets(one,hint,false);
    bool slow_found{};
    for(ULONGLONG now=0;now<=20000;now+=250) {
        auto batch=warm.round(preferred,true,now);
        require(!batch.empty() && batch.front().slot==4 && batch.size()<=2);
        if(std::ranges::any_of(batch,[](const Attempt& a){return a.slot==2 && a.ping_ms>=450;})) {
            slow_found=true;break;
        }
    }
    require(slow_found);
    std::cout << "Cold receiver slot 4 discovery model: legacy=" << latency(cold_slots)
              << "ms owner=" << cold_ms << "ms\n";
    std::cout << "Production discovery route parsing, ordering, stale fallback and list mode passed\n";
}
