#include "../app/src/g2h_thread_radio.hpp"
#include <cassert>
#include <functional>
#include <iostream>

// A byte-stream CLI peer, not a mocked SetEnabled implementation. It changes
// independent interface and Thread state only after the corresponding command.
struct RadioCli {
    int master=-1,keep=-1;
    std::string slave;
    std::atomic<bool> stop{false};
    std::thread worker;
    std::mutex mutex;
    std::vector<std::string> commands;
    std::string role="router",iface="up",fail;
    bool ignoreStop=false;
    std::function<void(const std::string &)> onCommand;
    RadioCli(){
        master=posix_openpt(O_RDWR|O_NOCTTY);assert(master>=0&&!grantpt(master)&&!unlockpt(master));
        slave=ptsname(master);keep=open(slave.c_str(),O_RDWR|O_NOCTTY);assert(keep>=0);
        worker=std::thread([this]{Run();});
    }
    ~RadioCli(){stop=true;worker.join();close(keep);close(master);}
    std::vector<std::string> Commands(){std::lock_guard<std::mutex> lock(mutex);return commands;}
    std::vector<std::string> Writes(){std::vector<std::string> out;for(auto &q:Commands())
        if(q=="thread stop"||q=="thread start"||q=="ifconfig up"||q=="ifconfig down")out.push_back(q);return out;}
    void Run(){
        std::string q;
        while(!stop){pollfd p{master,POLLIN,0};if(poll(&p,1,50)<=0)continue;
            char c;if(read(master,&c,1)!=1)continue;if(c=='\r')continue;
            if(c!='\n'){q+=c;continue;}if(q.empty())continue;
            {std::lock_guard<std::mutex> lock(mutex);commands.push_back(q);}
            if(onCommand)onCommand(q);
            std::string payload;
            if(q==fail)payload="Error 1: Failed";
            else if(q=="state")payload=role+"\r\nDone";
            else if(q=="ifconfig")payload=iface+"\r\nDone";
            else if(q=="thread stop"){if(!ignoreStop)role="disabled";payload="Done";}
            else if(q=="thread start"){role="detached";payload="Done";}
            else if(q=="ifconfig down"){iface="down";payload="Done";}
            else if(q=="ifconfig up"){iface="up";payload="Done";}
            else if(q=="channel")payload="25\r\nDone";
            else if(q=="networkname")payload="test-thread\r\nDone";
            else if(q=="panid")payload="0x421b\r\nDone";
            else if(q=="extpanid")payload="0123456789abcdef\r\nDone";
            else if(q=="extaddr")payload="fedcba9876543210\r\nDone";
            else if(q=="rloc16")payload="fffe\r\nDone";
            else payload="Error 7: InvalidArgs";
            const auto answer="stale asynchronous line\r\nDone\r\n> "+q+"\r\n"+payload+"\r\n> ";
            assert(write(master,answer.data(),answer.size())==ssize_t(answer.size()));q.clear();
        }
    }
};

int main(){
    using namespace g2h;
    const std::string lock="/tmp/g2h-radio-control-"+std::to_string(getpid());
    {
        RadioCli cli;ThreadRadio radio(cli.slave,lock);
        auto c=radio.ControlSnapshot();assert(!c.desired&&!c.observed&&!c.pending);
        assert(radio.PollOnce());c=radio.ControlSnapshot();assert(!c.desired&&c.observed&&*c.observed);
        assert(cli.Writes().empty()); // Construction and telemetry never change configuration.
        assert(radio.SetEnabled(false));c=radio.ControlSnapshot();assert(c.desired&&!*c.desired&&c.pending&&*c.observed);
        assert(cli.Writes().empty()); // Queuing on Matter thread does not perform UART I/O.
        assert(radio.PollOnce());c=radio.ControlSnapshot();
        assert(!c.pending&&c.error.empty()&&c.observed&&!*c.observed&&radio.Snapshot().Fresh());
        assert((cli.Writes()==std::vector<std::string>{"thread stop","ifconfig down"}));
        assert(radio.SetEnabled(true)&&radio.PollOnce());c=radio.ControlSnapshot();
        assert(!c.pending&&c.error.empty()&&c.observed&&*c.observed&&radio.Snapshot().role=="detached");
        assert((cli.Writes()==std::vector<std::string>{"thread stop","ifconfig down","ifconfig up","thread start"}));
        // No dataset/key/reset/GPIO/flash command is ever in the command stream.
        for(const auto &q:cli.Commands())assert(q.find("dataset")==std::string::npos&&q.find("reset")==std::string::npos);
    }
    {
        RadioCli cli;cli.fail="ifconfig down";ThreadRadio radio(cli.slave,lock);
        assert(radio.SetEnabled(false)&&!radio.PollOnce());const auto c=radio.ControlSnapshot();
        assert(c.desired&&!*c.desired&&!c.pending&&!c.observed&&c.error=="command-failed");
        assert((cli.Writes()==std::vector<std::string>{"thread stop","ifconfig down"}));
        // A future status poll does not blindly repeat a failed write.
        assert(radio.PollOnce());assert(cli.Writes().size()==2&&!radio.ControlSnapshot().observed);
    }
    {
        RadioCli cli;cli.ignoreStop=true;ThreadRadio radio(cli.slave,lock);
        assert(radio.SetEnabled(false)&&!radio.PollOnce());auto c=radio.ControlSnapshot();
        assert(!c.observed&&!c.pending&&c.error=="verification-failed"); // Done is not proof of the requested state.
    }
    {
        RadioCli cli;ThreadRadio radio(cli.slave,lock);assert(!mkdir(lock.c_str(),0700));
        assert(radio.SetEnabled(false)&&!radio.PollOnce());const auto c=radio.ControlSnapshot();
        assert(c.error=="uart-busy"&&!c.pending&&!c.observed&&cli.Commands().empty());
        assert(!rmdir(lock.c_str()));assert(radio.SetEnabled(false)&&radio.PollOnce());
    }
    {
        RadioCli cli;ThreadRadio radio(cli.slave,lock);radio.SetEnabled(false);radio.SetEnabled(true);
        assert(radio.PollOnce());assert((cli.Writes()==std::vector<std::string>{"ifconfig up","thread start"}));
        auto c=radio.ControlSnapshot();assert(c.generation==2&&!c.pending&&c.observed&&*c.observed);
    }
    {
        RadioCli cli;ThreadRadio radio(cli.slave,lock);
        // A request arriving during the older command must not be lost by its completion.
        cli.onCommand=[&](const std::string &q){if(q=="thread stop")radio.SetEnabled(true);};
        radio.SetEnabled(false);assert(radio.PollOnce());auto c=radio.ControlSnapshot();
        assert(c.pending&&c.desired&&*c.desired&&c.observed&&!*c.observed&&c.generation==2);
        assert(radio.PollOnce());c=radio.ControlSnapshot();assert(!c.pending&&c.observed&&*c.observed);
    }
    {
        RadioCli cli;ThreadRadio radio(cli.slave,lock);radio.Start();
        auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(!radio.Snapshot().Fresh()&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(5));
        assert(radio.Snapshot().Fresh());auto begin=std::chrono::steady_clock::now();radio.SetEnabled(false);
        while(radio.ControlSnapshot().pending&&std::chrono::steady_clock::now()-begin<std::chrono::seconds(2))std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto c=radio.ControlSnapshot();assert(!c.pending&&c.observed&&!*c.observed);radio.Stop();
    }
    std::cout<<"THREAD_CONTROL_OK\n";
}
