#pragma once
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>

namespace g2h {
// The vendor camera process's loopback debug server (127.0.0.1:101). A set goes
// through the vendor's own range check, apply callback and persistence, exactly
// as its other controllers use it. Original image/volume settings were verified
// on 2026-10-22; new native mute settings still require audio-effect qualification.
struct VendorSetting {const char *module,*name;int min,max;};
enum class Vendor : std::size_t {NightVision,StatusLight,Osd,Wdr,Brightness,SpeakerVolume,MicrophoneMuted,SpeakerMuted,Count};
inline constexpr std::array<VendorSetting,std::size_t(Vendor::Count)> kVendorSettings{{
    {"gpio","nightvision",0,2}, // 0 off (colour), 1 on (IR), 2 auto
    {"gpio","led",0,1},
    {"video","osd",0,1},
    {"video","wdr",0,1},
    {"video","brightness",0,100},
    {"audio","out_vol",0,100},
    {"audio","in_mute",0,1},
    {"audio","out_mute",0,1},
}};
inline constexpr bool IsAudioMute(Vendor which){return which==Vendor::MicrophoneMuted||which==Vendor::SpeakerMuted;}
inline constexpr bool IsVideoSetting(Vendor which){return which==Vendor::NightVision||which==Vendor::Osd||which==Vendor::Wdr||which==Vendor::Brightness;}

// 101 on the camera; host tests point it at a fake server.
inline std::uint16_t vendorDebugPort=101;
inline bool VendorExchange(const std::string &command,std::string &reply,std::chrono::milliseconds budget=std::chrono::seconds(4)){
    const auto until=std::chrono::steady_clock::now()+budget;
    auto left=[&]{return int(std::chrono::duration_cast<std::chrono::milliseconds>(until-std::chrono::steady_clock::now()).count());};
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return false;
    (void)fcntl(fd,F_SETFD,FD_CLOEXEC);
#ifdef MSG_NOSIGNAL
    constexpr int kSendFlags=MSG_NOSIGNAL;
#else
    constexpr int kSendFlags=0;int one=1;(void)setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof one);
#endif
    sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(vendorDebugPort);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    bool ok=connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof a)==0;
    if(ok){const std::string wire=command+'\0';ok=send(fd,wire.data(),wire.size(),kSendFlags)==ssize_t(wire.size())&&shutdown(fd,SHUT_WR)==0;}
    char buffer[128];
    while(ok){
        const int ms=left();if(ms<=0){ok=false;break;}
        pollfd p{fd,POLLIN,0};if(poll(&p,1,ms)<=0){ok=false;break;}
        const auto n=recv(fd,buffer,sizeof buffer,0);
        if(n<0){ok=false;break;}
        if(n==0)break;
        reply.append(buffer,std::size_t(n));if(reply.size()>256){ok=false;break;}
    }
    close(fd);return ok;
}
inline std::optional<int> VendorGet(const VendorSetting &s,std::chrono::milliseconds budget=std::chrono::seconds(4)){
    std::string reply;if(!VendorExchange(std::string(s.module)+" "+s.name,reply,budget))return std::nullopt;
    char *end=nullptr;const long value=std::strtol(reply.c_str(),&end,10);
    if(end==reply.c_str()||(*end&&*end!='\n')||value<s.min||value>s.max)return std::nullopt;
    return int(value);
}
inline bool VendorSet(const VendorSetting &s,int value){
    if(value<s.min||value>s.max)return false;
    std::string reply;const auto text=std::to_string(value);
    if(!VendorExchange(std::string(s.module)+" "+s.name+" "+text,reply))return false;
    return reply=="set "+std::string(s.module)+"_"+s.name+" "+text+"\n";
}

// Owns all traffic to the vendor debug server on one worker thread. The Matter
// thread only queues writes and collects observed values; it never blocks here.
// Values changed elsewhere (HomeKit, Aqara app) are picked up every 30 s.
class VendorControl {
public:
    static constexpr std::size_t kCount=std::size_t(Vendor::Count);
    ~VendorControl(){Stop();}
    void SetVideoSettingsAllowed(bool allowed){
        std::lock_guard<std::mutex> lock(mutex_);if(videoSettingsAllowed_==allowed)return;
        videoSettingsAllowed_=allowed;
        if(!allowed)for(std::size_t i=0;i<kCount;++i)if(IsVideoSetting(Vendor(i))&&hasPending_[i]){
            hasPending_[i]=false;known_[i]=false;changed_[i]=true;
        }
    }
    // Blocking initial read (startup only, bounded by 1 s per setting).
    void ReadAll(){
        for(std::size_t i=0;i<kCount;++i){
            auto v=VendorGet(kVendorSettings[i],std::chrono::seconds(1));
            std::lock_guard<std::mutex> lock(mutex_);if(v){value_[i]=*v;known_[i]=true;}
        }
    }
    bool Start(){
        std::lock_guard<std::mutex> lock(mutex_);if(started_)return true;
        stop_=false;pthread_attr_t attr;if(pthread_attr_init(&attr))return false;
        const bool ok=!pthread_attr_setstacksize(&attr,256*1024)&&!pthread_create(&thread_,&attr,Entry,this);
        pthread_attr_destroy(&attr);started_=ok;return ok;
    }
    void Stop() noexcept {
        {std::lock_guard<std::mutex> lock(mutex_);if(!started_)return;stop_=true;}
        cv_.notify_all();pthread_join(thread_,nullptr);started_=false;
    }
    std::optional<int> Value(Vendor which) const {
        std::lock_guard<std::mutex> lock(mutex_);const auto i=std::size_t(which);
        if(i>=kCount)return std::nullopt;
        return known_[i]?std::optional<int>(value_[i]):std::nullopt;
    }
    // Reports observed native settings, not the requested value or proof that
    // physical audio capture/playback stopped. Mixed settings remain unknown.
    std::optional<bool> AudioMuted() const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto mic=std::size_t(Vendor::MicrophoneMuted),speaker=std::size_t(Vendor::SpeakerMuted);
        if(!known_[mic]||!known_[speaker]||value_[mic]!=value_[speaker])return std::nullopt;
        return value_[mic]!=0;
    }
    // Queue both halves under one lock without changing either volume. The
    // vendor applies them separately; callers must gate their own media first.
    bool WriteAudioMuted(bool muted){
        {
            std::lock_guard<std::mutex> lock(mutex_);
            audioTarget_=muted;++audioGeneration_;
            nextAudioVerify_=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            for(auto which:{Vendor::MicrophoneMuted,Vendor::SpeakerMuted}){
                const auto i=std::size_t(which);pending_[i]=muted?1:0;hasPending_[i]=true;
                known_[i]=false;changed_[i]=true;
            }
        }
        cv_.notify_all();return true;
    }
    // Queue a write; the latest value per setting wins. Out of range is refused.
    bool Write(Vendor which,int value){
        const auto i=std::size_t(which);if(i>=kCount)return false;const auto &s=kVendorSettings[i];
        if(value<s.min||value>s.max)return false;
        {std::lock_guard<std::mutex> lock(mutex_);
            if(IsVideoSetting(which)&&!videoSettingsAllowed_)return false;
            pending_[i]=value;hasPending_[i]=true;
            if(IsAudioMute(which)){known_[i]=false;changed_[i]=true;}
            else{value_[i]=value;known_[i]=true;}}
        cv_.notify_all();return true;
    }
    // Settings whose vendor value changed outside Matter since the last call.
    std::array<bool,kCount> TakeChanges(){
        std::lock_guard<std::mutex> lock(mutex_);auto out=changed_;changed_={};return out;
    }
private:
    static void *Entry(void *self){static_cast<VendorControl*>(self)->Run();return nullptr;}
    void Run() noexcept {
        auto nextPoll=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        std::unique_lock<std::mutex> lock(mutex_);
        while(!stop_){
            const auto wake=audioTarget_&&nextAudioVerify_<nextPoll?nextAudioVerify_:nextPoll;
            cv_.wait_until(lock,wake,[&]{if(stop_)return true;for(bool p:hasPending_)if(p)return true;return false;});
            if(stop_)break;
            for(std::size_t i=0;i<kCount;++i){
                if(!hasPending_[i])continue;
                const int value=pending_[i];hasPending_[i]=false;
                lock.unlock();const bool ok=VendorSet(kVendorSettings[i],value);
                // The native setter acknowledgement ignores driver failures.
                // Read back mutes immediately; never publish a queued request
                // as applied. Actual audio-effect validation is separate.
                auto observed=IsAudioMute(Vendor(i))?VendorGet(kVendorSettings[i],std::chrono::seconds(1)):std::optional<int>{};
                lock.lock();
                if(IsAudioMute(Vendor(i))){
                    if(!hasPending_[i]){
                        known_[i]=ok&&observed&&*observed==value;
                        if(known_[i])value_[i]=*observed;
                        changed_[i]=true;
                    }
                }else if(!ok){known_[i]=false;changed_[i]=true;} // re-read at the next poll
            }
            const auto mic=std::size_t(Vendor::MicrophoneMuted),speaker=std::size_t(Vendor::SpeakerMuted);
            // A confirmed ON request releases enforcement, allowing later
            // external mutes. OFF remains authoritative across native resets.
            if(audioTarget_==std::optional<bool>(false)&&known_[mic]&&known_[speaker]&&
               value_[mic]==0&&value_[speaker]==0&&!hasPending_[mic]&&!hasPending_[speaker])audioTarget_.reset();
            if(audioTarget_&&std::chrono::steady_clock::now()>=nextAudioVerify_){
                const int target=*audioTarget_?1:0;const auto generation=audioGeneration_;
                nextAudioVerify_=std::chrono::steady_clock::now()+std::chrono::seconds(2);
                for(auto i:{mic,speaker}){
                    lock.unlock();auto observed=VendorGet(kVendorSettings[i],std::chrono::seconds(1));lock.lock();
                    if(generation!=audioGeneration_)break;
                    if(hasPending_[i])continue;
                    if(!observed||*observed!=target){
                        known_[i]=false;changed_[i]=true;pending_[i]=target;hasPending_[i]=true;
                    }else if(!known_[i]||value_[i]!=*observed){known_[i]=true;value_[i]=*observed;changed_[i]=true;}
                }
            }
            if(std::chrono::steady_clock::now()<nextPoll)continue;
            nextPoll=std::chrono::steady_clock::now()+std::chrono::seconds(30);
            for(std::size_t i=0;i<kCount&&!stop_;++i){
                if(hasPending_[i])continue;
                lock.unlock();auto v=VendorGet(kVendorSettings[i],std::chrono::seconds(1));lock.lock();
                if(hasPending_[i])continue;
                if(!v){
                    if(IsAudioMute(Vendor(i))&&known_[i]){known_[i]=false;changed_[i]=true;}
                    continue;
                }
                if(IsAudioMute(Vendor(i))&&audioTarget_==std::optional<bool>(true)&&*v!=1){
                    known_[i]=false;changed_[i]=true;pending_[i]=1;hasPending_[i]=true;continue;
                }
                if(!known_[i]||value_[i]!=*v){value_[i]=*v;known_[i]=true;changed_[i]=true;}
            }
        }
    }
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    pthread_t thread_{};
    bool started_=false,stop_=false,videoSettingsAllowed_=true;
    std::optional<bool> audioTarget_;std::uint64_t audioGeneration_=0;
    std::chrono::steady_clock::time_point nextAudioVerify_{};
    std::array<int,kCount> value_{},pending_{};
    std::array<bool,kCount> known_{},hasPending_{},changed_{};
};
}
