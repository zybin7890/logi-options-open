#include "hid_diversion.hpp"
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <new>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
constexpr unsigned button=0x53, untouched=0x56, foreign=0xc3, thumb=0x10000;
struct Registers {unsigned side=0, other=0, gesture=0, wheel=1;};
}

int main(){
    char path[]="/tmp/logi-diversion-test-XXXXXX";
    if(!mkdtemp(path))return 1;
    struct Cleanup {const char* path;~Cleanup(){std::error_code ec;std::filesystem::remove_all(path,ec);}} cleanup{path};
    try {
        auto* shared=mmap(nullptr,sizeof(Registers),PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_SHARED,-1,0);
        require(shared!=MAP_FAILED,"Cannot allocate simulated device registers");
        auto& hw=*new(shared) Registers;
        int ready[2];
        require(pipe(ready)==0,"Cannot create checkpoint pipe");
        auto child=fork();
        require(child>=0,"Cannot fork worker");
        if(child==0) {
            close(ready[0]);
            try {
                loo_linux::HidDiversions owner;
                owner.bind(path,1234);
                owner.remember(button,hw.side,1); hw.side=1;
                owner.remember(thumb,hw.wheel,256); hw.wheel=256;
                owner.remember(foreign,hw.gesture,17); hw.gesture=17;
                // Die between the write-ahead record and one hardware write.
                owner.remember(untouched,hw.other,1);
                if(write(ready[1],"r",1)!=1)_exit(2);
                for(;;)pause();
            }catch(...){_exit(3);}
        }
        close(ready[1]);
        char checkpoint{};
        const bool issued=read(ready[0],&checkpoint,1)==1;
        close(ready[0]);
        kill(child,SIGKILL);
        int status{};
        require(waitpid(child,&status,0)==child && issued && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL,
                "Worker did not die with active HID diversions");
        require(hw.side==1 && hw.wheel==256,"Simulated device lost its state when its worker died");
        require(loo_linux::HidDiversions::any_pending(path),"Crash cleanup failed to detect retained diversion");
        loo_linux::HidDiversions next;
        next.bind(path,5678);
        require(next.empty(),"Adopted a different mouse's diversions");
        next.bind(path,1234);
        hw.gesture=1; // Another owner changed this control after the crash.
        unsigned writes{};
        auto state=[&](unsigned id)->unsigned&{
            switch(id){case button:return hw.side;case untouched:return hw.other;case foreign:return hw.gesture;case thumb:return hw.wheel;}
            throw std::runtime_error("Unexpected register");
        };
        auto read=[&](unsigned id)->std::optional<unsigned>{return state(id);};
        bool refused=false;
        try {
            next.restore(read,[&](unsigned id,unsigned original) {
                ++writes;
                if(id!=thumb)state(id)=original;
                return true; // The wheel's device write acknowledges without applying.
            });
        }catch(const std::runtime_error&){refused=true;}
        require(refused && !next.empty() && writes==2 && hw.side==0 && hw.wheel==256 &&
                hw.gesture==1 && hw.other==0,"Recovery lost ownership boundaries or accepted failed readback");
        loo_linux::HidDiversions retry;
        retry.bind(path,1234);
        retry.restore(read,[&](unsigned id,unsigned original){
            require(id==thumb,"Retried a control already restored");
            state(id)=original; return true;
        });
        require(hw.wheel==1 && retry.empty() && std::filesystem::is_empty(path) &&
                !loo_linux::HidDiversions::any_pending(path),"Incomplete journal cleanup");
        munmap(shared,sizeof(Registers));
        std::cout<<"SIGKILL retained simulated HID diversion; matching registers restored and verified.\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
