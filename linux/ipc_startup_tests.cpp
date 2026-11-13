#include "agent.hpp"
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace loo_linux;
using Clock=std::chrono::steady_clock;
namespace {
void require(bool okay,const char* why){if(!okay)throw std::runtime_error(why);}
struct Server {
    pid_t pid{-1};
    Server(const std::filesystem::path& directory,unsigned bind_delay,unsigned reply_delay,bool stale=false){
        int pipefd[2];require(pipe(pipefd)==0,"pipe failed");
        pid=fork();require(pid>=0,"fork failed");
        if(pid==0){
            close(pipefd[0]);
            const auto lock=open((directory/"agent.lock").c_str(),O_CREAT|O_RDWR,0600);
            if(lock<0||flock(lock,LOCK_EX|LOCK_NB))_exit(2);
            sockaddr_un address{};address.sun_family=AF_UNIX;
            const auto path=(directory/"control.sock").string();
            std::memcpy(address.sun_path,path.c_str(),path.size()+1);
            unlink(path.c_str());
            if(stale){
                const auto old=socket(AF_UNIX,SOCK_SEQPACKET,0);
                if(old<0||bind(old,reinterpret_cast<sockaddr*>(&address),sizeof(address)))_exit(3);
                close(old);
            }
            if(write(pipefd[1],"L",1)!=1)_exit(4);close(pipefd[1]);
            std::this_thread::sleep_for(std::chrono::milliseconds(bind_delay));
            unlink(path.c_str());const auto listener=socket(AF_UNIX,SOCK_SEQPACKET,0);
            if(listener<0||bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))||listen(listener,4))_exit(5);
            const auto client=accept(listener,nullptr,nullptr);if(client<0)_exit(6);
            char request[128]{};const auto count=recv(client,request,sizeof(request),0);
            if(count!=12||std::string_view(request,count)!="agent-status")_exit(7);
            std::this_thread::sleep_for(std::chrono::milliseconds(reply_delay));
            constexpr char response[]="OK\n{\"native\":true,\"ready\":false}";
            send(client,response,sizeof(response)-1,MSG_NOSIGNAL);
            close(client);close(listener);unlink(path.c_str());close(lock);_exit(0);
        }
        close(pipefd[1]);char ready{};
        const auto count=read(pipefd[0],&ready,1);close(pipefd[0]);
        require(count==1&&ready=='L',"server did not acquire startup lock");
    }
    ~Server(){if(pid>0){kill(pid,SIGTERM);waitpid(pid,nullptr,0);}}
    void join(){int status{};require(waitpid(pid,&status,0)==pid,"waitpid failed");pid=-1;
        require(WIFEXITED(status)&&WEXITSTATUS(status)==0,"fixture server failed");}
};
}
int main(){
    char temporary[]="/tmp/loo-ipc-start-XXXXXX";const auto root=mkdtemp(temporary);if(!root)return 1;
    setenv("XDG_RUNTIME_DIR",root,1);
    const auto directory=std::filesystem::path(root)/("logi-options-open-"+std::to_string(getuid()));
    mkdir(directory.c_str(),0700);
    try{
        for(bool stale:{false,true}){
            Server server(directory,120,0,stale);
            require(dispatch("agent-status")=="{\"native\":true,\"ready\":false}","startup acknowledgement lost");
            server.join();
        }
        {
            Server server(directory,300,0);bool rejected=false;
            try{transact("agent-status",40);}catch(const std::exception& e){rejected=std::string(e.what()).find("not sent")!=std::string::npos;}
            require(rejected,"startup timeout did not distinguish an unsent command");
        }
        {
            Server server(directory,80,200);bool unknown=false;const auto start=Clock::now();
            try{transact("agent-status",140);}catch(const std::exception& e){unknown=std::string(e.what()).find("completion is unknown")!=std::string::npos;}
            require(unknown,"post-send timeout must not replay the request");
            require(Clock::now()-start<std::chrono::milliseconds(240),"startup and reply exceeded their shared deadline");
            server.join();
        }
        const auto start=Clock::now();bool unavailable=false;
        try{transact("agent-status",30000);}catch(const std::exception& e){unavailable=std::string(e.what()).find("unavailable")!=std::string::npos;}
        require(unavailable&&Clock::now()-start<std::chrono::milliseconds(500),"absent owner should fail promptly");
        std::filesystem::remove_all(root);
        std::cout<<"PASS: production IPC waits for lock-before-listen and stale sockets; shared timeout, no replay, absent owner.\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<"; artifacts: "<<root<<'\n';return 1;}
}
