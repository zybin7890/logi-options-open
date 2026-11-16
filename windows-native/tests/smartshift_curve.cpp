#include "../native_smartshift.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

int main() {
    using namespace native_smartshift;
    const auto check=[](bool ok){if(!ok)throw std::runtime_error("SmartShift original curve mismatch");};
    constexpr std::array<std::pair<unsigned,unsigned>,7> values{{{0,50},{1,50},{25,38},{50,27},{75,15},{83,12},{100,4}}};
    for(const auto& [p,raw]:values)check(threshold(p)==raw);
    check(percent(4)==100 && percent(12)==82 && percent(15)==76);
    check(percent(27)==50 && percent(38)==26 && percent(50)==0);
    check(display(12,83)==83 && display(12,{})==82);
    check(display(50,0)==0); // 0 percent is enabled; raw zero is disabled.
    check(display(0,75)==75 && display(0,{})==83);
    const std::wstring original=L"# personal\n[global]\n0053 = MouseBack\n";
    const auto saved=write(original,83);
    check(read(saved)==83 && saved.ends_with(original));
    check(read(write(saved,0))==0 && write(saved,83)==saved);
    check(!read(L"# UI smartshift.sensitivity=101\n").has_value());
    std::cout<<"Original SmartShift thresholds, disabled separation and persistence passed\n";
}
