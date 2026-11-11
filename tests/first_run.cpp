#include "../first_run.hpp"
#include <chrono>
#include <iostream>
int main() {
    auto root=std::filesystem::temp_directory_path()/
        ("loo-first-run-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto path=root/"disclaimer-v1.accepted";
    if(!loo::notice_required(path)) return 1;
    if(!loo::accept_notice(path) || loo::notice_required(path)) return 2;
    {std::ofstream out(path);out<<"wrong-version\n";}
    if(!loo::notice_required(path)) return 3;
    // Unwritable target must not be recorded as an acknowledgement.
    if(loo::accept_notice(path/"child")) return 4;
    std::filesystem::remove_all(root);
    std::cout<<"First-use notice: missing, accepted, corrupt, failed write passed.\n";
}
