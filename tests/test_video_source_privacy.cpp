// Synthetic private-broker input only. No vendor process or native RTSP client.
#define G2H_VIDEO_SOURCE_TESTING
#include "../app/src/g2h_video_source.hpp"
#include <cassert>
#include <condition_variable>
#include <cstdlib>

using namespace std::chrono;
using namespace g2h;
static void WaitFor(const std::function<bool()> &predicate){
    const auto until=Clock::now()+seconds(1);
    for(;;){if(predicate())return;assert(Clock::now()<until);std::this_thread::sleep_for(milliseconds(2));}
}
static std::vector<uint8_t> Packet(unsigned seq,unsigned stamp,std::initializer_list<uint8_t> payload,bool marker=true){
    std::vector<uint8_t> out{0x80,uint8_t(36|(marker?128:0)),uint8_t(seq>>8),uint8_t(seq),
        uint8_t(stamp>>24),uint8_t(stamp>>16),uint8_t(stamp>>8),uint8_t(stamp),0,0,0,1};
    out.insert(out.end(),payload);return out;
}
int main(int argc,char **argv){
    assert(argc==2);std::string directory=argv[1],path=directory+"/video.sock";
    assert(!setenv("G2H_MEDIA_BROKER_DIR",directory.c_str(),1));
    int listener=socket(AF_UNIX,SOCK_STREAM,0);assert(listener>=0);
    sockaddr_un address{};address.sun_family=AF_UNIX;assert(path.size()<sizeof(address.sun_path));
    std::memcpy(address.sun_path,path.c_str(),path.size()+1);
    const int bound=bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address));
    if(bound)std::perror("synthetic broker bind");assert(!bound);
    assert(!chmod(path.c_str(),0600)&&!listen(listener,1));
    std::atomic<int> peer{-1};
    std::thread server([&]{
        const int client=accept(listener,nullptr,nullptr);assert(client>=0);peer=client;
        media::Info info;info.payload=36;info.sps={0x67,0x42,0x40,0x1f,0x97,0x40,0x28,0x02,0xdd,0x36,0x02};info.pps={0x68,0xce,0x3c,0x80};
        const auto data=info.Encode();media::Send(client,data.data(),data.size(),milliseconds(500));
    });
    VideoSource source;
    std::mutex mutex;std::condition_variable cv;bool block=false,entered=false,released=false;
    source.beforePayloadRead=[&]{
        std::unique_lock<std::mutex> lock(mutex);if(!block)return;
        entered=true;cv.notify_all();cv.wait(lock,[&]{return released;});block=false;
    };
    assert(source.Start());server.join();assert(peer>=0);source.EnableFrames(true);
    auto send=[&](const std::vector<uint8_t> &packet){
        std::vector<uint8_t> record{'$',0,uint8_t(packet.size()>>8),uint8_t(packet.size())};
        record.insert(record.end(),packet.begin(),packet.end());media::Send(peer,record.data(),record.size(),milliseconds(500));
    };
    std::vector<uint8_t> idr;H264Rtp::Frame frame;
    send(Packet(1,1,{0x65,11}));WaitFor([&]{return source.LatestIdr(idr);});
    assert(idr.back()==11);WaitFor([&]{return source.Pop(frame);});assert(frame.idr);
    source.SetGlobalAllowed(false);const auto epoch=source.Epoch();
    assert(!source.GlobalAllowed()&&!source.LatestIdr(idr)&&!source.Pop(frame));
    source.SetGlobalAllowed(false);assert(source.Epoch()==epoch);
    send(Packet(2,2,{0x65,12}));std::this_thread::sleep_for(milliseconds(20));
    assert(!source.LatestIdr(idr)&&!source.Pop(frame));
    source.SetGlobalAllowed(true);assert(source.Epoch()==epoch+1);
    // An inter frame cannot restart delivery after invalidation.
    send(Packet(3,3,{0x61,13}));std::this_thread::sleep_for(milliseconds(20));assert(!source.Pop(frame));
    send(Packet(4,4,{0x65,14}));WaitFor([&]{return source.Pop(frame);});assert(frame.annexB.back()==14);
    // Block after the header, with the epoch already captured, then do off/on.
    {std::lock_guard<std::mutex> lock(mutex);block=true;entered=false;released=false;}
    send(Packet(5,5,{0x65,15}));
    {std::unique_lock<std::mutex> lock(mutex);assert(cv.wait_for(lock,seconds(1),[&]{return entered;}));}
    source.SetGlobalAllowed(false);source.SetGlobalAllowed(true);
    {std::lock_guard<std::mutex> lock(mutex);released=true;}cv.notify_all();
    std::this_thread::sleep_for(milliseconds(20));assert(!source.LatestIdr(idr)&&!source.Pop(frame));
    send(Packet(6,6,{0x65,16}));WaitFor([&]{return source.Pop(frame);});assert(frame.annexB.back()==16);
    // A fragmented IDR started before the boundary cannot complete afterwards.
    send(Packet(7,7,{0x7c,0x85,17},false));
    std::this_thread::sleep_for(milliseconds(20));source.SetGlobalAllowed(false);source.SetGlobalAllowed(true);
    send(Packet(8,7,{0x7c,0x45,18}));std::this_thread::sleep_for(milliseconds(20));
    assert(!source.LatestIdr(idr)&&!source.Pop(frame));
    send(Packet(9,8,{0x65,19}));WaitFor([&]{return source.Pop(frame);});assert(frame.annexB.back()==19);
    // A real idle capture pause exceeds the ordinary 2-second record budget.
    source.SetGlobalAllowed(false);std::this_thread::sleep_for(milliseconds(2300));
    assert(!source.Faulted()&&!source.LatestIdr(idr));source.SetGlobalAllowed(true);
    // First old-epoch record may be dropped; following fresh IDR recovers.
    send(Packet(10,9,{0x65,20}));send(Packet(11,10,{0x65,21}));
    WaitFor([&]{return source.Pop(frame);});assert(frame.annexB.back()==21);
    // Pause after only one byte of a genuine interleaved header.
    const auto split=Packet(12,11,{0x65,22});
    const uint8_t first='$';media::Send(peer,&first,1,milliseconds(500));
    std::this_thread::sleep_for(milliseconds(20));source.SetGlobalAllowed(false);
    std::this_thread::sleep_for(milliseconds(2300));assert(!source.Faulted());
    source.SetGlobalAllowed(true);
    std::vector<uint8_t> remainder{0,uint8_t(split.size()>>8),uint8_t(split.size())};
    remainder.insert(remainder.end(),split.begin(),split.end());media::Send(peer,remainder.data(),remainder.size(),milliseconds(500));
    send(Packet(13,12,{0x65,23}));WaitFor([&]{return source.Pop(frame);});assert(frame.annexB.back()==23);
    // Cancellation stays bounded while paused and no peer data can arrive.
    source.SetGlobalAllowed(false);auto stoppedAt=Clock::now();source.Stop();
    // Allow host scheduler/ASan teardown slack. An unbounded paused read
    // still fails independently of the normal two-second media budget.
    assert(Clock::now()-stoppedAt<seconds(1));assert(!source.Faulted());
    // Gates never closed/replaced even the broker subscriber; no reconnection.
    pollfd listenerState{listener,POLLIN,0};assert(poll(&listenerState,1,0)==0);
    assert(!source.Faulted());shutdown(peer,SHUT_RDWR);source.Stop();close(peer);
    // Pausing must not remove ordinary corruption/deadline boundaries.
    auto handshake=[&]{
        const int client=accept(listener,nullptr,nullptr);assert(client>=0);peer=client;
        media::Info info;info.payload=36;info.sps={0x67,0x42,0x40,0x1f,0x97,0x40,0x28,0x02,0xdd,0x36,0x02};info.pps={0x68,0xce,0x3c,0x80};
        const auto data=info.Encode();media::Send(client,data.data(),data.size(),milliseconds(500));
    };
    VideoSource unpaused;std::thread nextPeer(handshake);assert(unpaused.Start());nextPeer.join();
    std::this_thread::sleep_for(milliseconds(2300));assert(unpaused.Faulted());unpaused.Stop();close(peer);
    VideoSource malformed;malformed.SetGlobalAllowed(false);std::thread badPeer(handshake);assert(malformed.Start());badPeer.join();
    const uint8_t badHeader[4]={'?',0,0,1};media::Send(peer,badHeader,sizeof(badHeader),milliseconds(500));
    WaitFor([&]{return malformed.Faulted();});malformed.Stop();close(peer);
    close(listener);unlink(path.c_str());
    std::puts("video source privacy tests passed");
}
