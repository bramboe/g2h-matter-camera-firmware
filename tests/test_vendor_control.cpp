// Native mute acknowledgement is not proof of an applied setting. Exercise
// asynchronous write/readback against a deliberately dishonest debug server.
#include "g2h_vendor_control.hpp"
#include <cassert>
#include <cstdio>
#include <map>
#include <thread>
using namespace std::chrono_literals;

template<class F>static void Until(F f){
    const auto end=std::chrono::steady_clock::now()+3s;
    while(!f()){assert(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(2ms);}
}
struct FakeVendor {
    int fd=-1;std::atomic<bool> stop{false};std::atomic<unsigned> sets{0},muteReads{0};std::thread thread;
    std::mutex mutex;
    std::map<std::string,int> values{{"gpio nightvision",2},{"gpio led",1},{"video osd",1},{"video wdr",0},
        {"video brightness",50},{"audio out_vol",80},{"audio in_mute",0},{"audio out_mute",0}};
    std::string ignoreWrite,invalidRead;
    FakeVendor(){
        fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        assert(!bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof a));socklen_t size=sizeof a;
        assert(!getsockname(fd,reinterpret_cast<sockaddr*>(&a),&size));g2h::vendorDebugPort=ntohs(a.sin_port);assert(!listen(fd,8));
        thread=std::thread([this]{Run();});
    }
    ~FakeVendor(){stop=true;thread.join();close(fd);}
    void Run(){
        while(!stop){
            pollfd p{fd,POLLIN,0};if(poll(&p,1,20)<=0)continue;int client=accept(fd,nullptr,nullptr);assert(client>=0);
            std::string command;char bytes[128];ssize_t size;
            while((size=recv(client,bytes,sizeof bytes,0))>0)command.append(bytes,std::size_t(size));
            assert(!command.empty()&&command.back()=='\0');command.pop_back();
            const auto first=command.find(' '),second=command.find(' ',first+1);
            const auto key=command.substr(0,second);std::string response;
            {
                std::lock_guard<std::mutex> lock(mutex);assert(values.count(key));
                if(second==std::string::npos){
                    response=key==invalidRead?"not-an-integer":std::to_string(values.at(key));
                    if(key=="audio in_mute"||key=="audio out_mute")++muteReads;
                }else{
                    const auto value=command.substr(second+1);if(key!=ignoreWrite)values.at(key)=std::stoi(value);
                    auto ackKey=key;ackKey[first]='_';response="set "+ackKey+" "+value+"\n";++sets;
                }
            }
            assert(send(client,response.data(),response.size(),0)==ssize_t(response.size()));close(client);
        }
    }
};

int main(){
    FakeVendor server;
    {
        g2h::VendorControl control;control.ReadAll();
        assert(control.Write(g2h::Vendor::Wdr,1));
        control.SetVideoSettingsAllowed(false);
        assert(!control.Write(g2h::Vendor::Wdr,1)&&!control.Write(g2h::Vendor::Brightness,80));
        assert(!control.Write(g2h::Vendor::NightVision,1)&&!control.Write(g2h::Vendor::Osd,0));
        assert(control.WriteAudioMuted(true)&&control.Start());Until([&]{return control.AudioMuted()==true;});
        {std::lock_guard<std::mutex> lock(server.mutex);assert(server.values.at("video wdr")==0&&server.values.at("video brightness")==50);}
        assert(control.WriteAudioMuted(false));Until([&]{return control.AudioMuted()==false;});
        control.SetVideoSettingsAllowed(true);assert(control.Write(g2h::Vendor::Wdr,1));
        Until([&]{std::lock_guard<std::mutex> lock(server.mutex);return server.values.at("video wdr")==1;});
        assert(control.Write(g2h::Vendor::Wdr,0));Until([&]{std::lock_guard<std::mutex> lock(server.mutex);return server.values.at("video wdr")==0;});control.Stop();
    }
    {
        const auto sets=server.sets.load();
        g2h::VendorControl control;control.ReadAll();assert(server.sets==sets);
        assert(control.AudioMuted()==false&&control.Value(g2h::Vendor::SpeakerVolume)==80);
        assert(!control.Write(g2h::Vendor::MicrophoneMuted,2)&&!control.Write(g2h::Vendor::SpeakerMuted,-1));
        assert(!control.Write(g2h::Vendor::Count,1)&&!control.Value(g2h::Vendor::Count));
        // Queued requests have no optimistic success, including before Start.
        assert(control.WriteAudioMuted(true));assert(!control.AudioMuted());
        assert(!control.Value(g2h::Vendor::MicrophoneMuted)&&!control.Value(g2h::Vendor::SpeakerMuted));
        assert(control.Start());Until([&]{return control.AudioMuted()==true;});
        {std::lock_guard<std::mutex> lock(server.mutex);assert(server.values.at("audio out_vol")==80);}
        assert(control.WriteAudioMuted(false));Until([&]{return control.AudioMuted()==false;});
        control.Stop();
    }
    // Native boot initialization can reset already-acknowledged mutes.
    // Keep global OFF enforced, but release enforcement after confirmed ON.
    {
        g2h::VendorControl control;control.ReadAll();
        assert(control.WriteAudioMuted(true)&&control.Start());Until([&]{return control.AudioMuted()==true;});
        const auto writes=server.sets.load();
        {std::lock_guard<std::mutex> lock(server.mutex);server.values.at("audio in_mute")=0;server.values.at("audio out_mute")=0;}
        Until([&]{return server.sets>=writes+2;});Until([&]{return control.AudioMuted()==true;});
        {std::lock_guard<std::mutex> lock(server.mutex);assert(server.values.at("audio in_mute")==1&&server.values.at("audio out_mute")==1&&server.values.at("audio out_vol")==80);}
        assert(control.WriteAudioMuted(false));Until([&]{return control.AudioMuted()==false;});
        {std::lock_guard<std::mutex> lock(server.mutex);server.values.at("audio in_mute")=1;server.values.at("audio out_mute")=1;}
        const auto onWrites=server.sets.load();std::this_thread::sleep_for(2200ms);
        assert(server.sets==onWrites);control.Stop();
        {std::lock_guard<std::mutex> lock(server.mutex);server.values.at("audio in_mute")=0;server.values.at("audio out_mute")=0;}
    }
    // A success-shaped native acknowledgement without applying microphone mute
    // cannot be exposed as successful mute, even when speaker mute succeeds.
    {
        {std::lock_guard<std::mutex> lock(server.mutex);server.ignoreWrite="audio in_mute";}
        g2h::VendorControl control;control.ReadAll();const auto reads=server.muteReads.load();
        assert(control.WriteAudioMuted(true));assert(control.Start());
        Until([&]{return server.muteReads>=reads+2;});Until([&]{return control.Value(g2h::Vendor::SpeakerMuted)==1;});
        assert(!control.AudioMuted()&&!control.Value(g2h::Vendor::MicrophoneMuted));control.Stop();
    }
    // Invalid readback remains unknown. ReadAll does not overwrite a persisted
    // mixed starting state and never changes the speaker volume.
    {
        {std::lock_guard<std::mutex> lock(server.mutex);server.ignoreWrite.clear();server.invalidRead="audio out_mute";}
        g2h::VendorControl control;const auto sets=server.sets.load();control.ReadAll();assert(server.sets==sets);
        assert(!control.AudioMuted());const auto reads=server.muteReads.load();
        assert(control.WriteAudioMuted(false));assert(control.Start());Until([&]{return server.muteReads>=reads+2;});
        Until([&]{return control.Value(g2h::Vendor::MicrophoneMuted)==0;});assert(!control.AudioMuted());control.Stop();
        std::lock_guard<std::mutex> lock(server.mutex);assert(server.values.at("audio out_vol")==80);
    }
    std::puts("Native audio mute startup, queued-state, readback and failure tests passed");
}
