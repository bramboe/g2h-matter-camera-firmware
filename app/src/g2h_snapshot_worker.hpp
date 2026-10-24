#pragma once
#include "g2h_snapshot_dimensions.hpp"
#include <pthread.h>
#include <stdexcept>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace g2h {
// Decodes and encodes snapshots off the Matter event thread. That thread only
// copies a cached JPEG or waits a caller-chosen bounded time for one, so a live
// WebRTC session keeps its frame cadence. Bounded RAM: one JPEG, one request.
class SnapshotWorker {
public:
    using SteadyClock=std::chrono::steady_clock;
    // Latest IDR access unit and the time it was received; false when none is fresh.
    using Source=std::function<bool(std::vector<std::uint8_t>&,SteadyClock::time_point&)>;
    using SizedEncode=std::function<std::vector<std::uint8_t>(const std::vector<std::uint8_t>&,unsigned,unsigned,unsigned)>;
    using Encode=std::function<std::vector<std::uint8_t>(const std::vector<std::uint8_t>&,unsigned)>;
    SnapshotWorker(Source source,SizedEncode encode,std::chrono::milliseconds maxAge=std::chrono::seconds(3),
                   std::chrono::milliseconds requestBudget=std::chrono::milliseconds(2500))
        :source_(std::move(source)),encode_(std::move(encode)),maxAge_(maxAge),budget_(requestBudget){}
    SnapshotWorker(Source source,Encode encode,std::chrono::milliseconds maxAge=std::chrono::seconds(3),
                   std::chrono::milliseconds requestBudget=std::chrono::milliseconds(2500))
        :SnapshotWorker(std::move(source),SizedEncode([encode=std::move(encode)](const std::vector<std::uint8_t>&idr,unsigned q,unsigned w,unsigned h){
            if(w!=320||h!=180)throw std::invalid_argument("legacy snapshot size");return encode(idr,q);
        }),maxAge,requestBudget){}
    ~SnapshotWorker(){Stop();}
    SnapshotWorker(const SnapshotWorker&)=delete;
    SnapshotWorker& operator=(const SnapshotWorker&)=delete;
    bool Start(){
        std::lock_guard<std::mutex> lock(mutex_);
        if(started_)return true;
        stop_=false;pthread_attr_t attr;
        if(pthread_attr_init(&attr))return false;
        // An explicit 1 MiB stack keeps this thread small under the app's RLIMIT_AS.
        const bool ok=!pthread_attr_setstacksize(&attr,1024*1024)&&!pthread_create(&thread_,&attr,Entry,this);
        pthread_attr_destroy(&attr);started_=ok;return ok;
    }
    void Stop() noexcept {
        {std::lock_guard<std::mutex> lock(mutex_);if(!started_)return;stop_=true;}
        cv_.notify_all();pthread_join(thread_,nullptr);
        std::lock_guard<std::mutex> lock(mutex_);started_=false;pending_=false;jpeg_.clear();failedAt_={};
    }
    // Non-blocking: begin preparing a JPEG at this quality.
    void Request(unsigned quality,unsigned width=320,unsigned height=180){if(!Valid(quality,width,height))return;std::lock_guard<std::mutex> lock(mutex_);if(started_&&!stop_)RequestLocked(quality,width,height);}
    bool Get(unsigned quality,std::chrono::milliseconds wait,std::vector<std::uint8_t>&out,unsigned width=320,unsigned height=180){
        if(!Valid(quality,width,height))return false;
        std::unique_lock<std::mutex> lock(mutex_);
        if(!started_||stop_)return false;
        if(!FreshLocked(quality,width,height)){
            RequestLocked(quality,width,height);
            cv_.wait_for(lock,wait,[&]{return stop_||FreshLocked(quality,width,height);});
            if(stop_||!FreshLocked(quality,width,height))return false;
        }
        out=jpeg_;return true;
    }
    unsigned Encodes() const {std::lock_guard<std::mutex> lock(mutex_);return encodes_;}
private:
    static void *Entry(void *self){try{static_cast<SnapshotWorker*>(self)->Run();}catch(...){}return nullptr;}
    static bool Valid(unsigned q,unsigned w,unsigned h){return q>=1&&q<=100&&SnapshotDimensions(w,h);}
    bool FreshLocked(unsigned quality,unsigned width,unsigned height) const {
        return !jpeg_.empty()&&quality_==quality&&width_==width&&height_==height&&SteadyClock::now()-idrAt_<=maxAge_;
    }
    void RequestLocked(unsigned quality,unsigned width,unsigned height){
        const auto until=SteadyClock::now()+budget_;
        if(!pending_||wantQuality_!=quality||wantWidth_!=width||wantHeight_!=height||until>requestUntil_)requestUntil_=until;
        wantQuality_=quality;wantWidth_=width;wantHeight_=height;pending_=true;cv_.notify_all();
    }
    void Run(){
        std::unique_lock<std::mutex> lock(mutex_);
        while(!stop_){
            cv_.wait(lock,[&]{return stop_||pending_;});
            if(stop_)break;
            const unsigned quality=wantQuality_,width=wantWidth_,height=wantHeight_;
            if(FreshLocked(quality,width,height)||SteadyClock::now()>=requestUntil_){pending_=false;cv_.notify_all();continue;}
            const auto failedAt=(failedQuality_==quality&&failedWidth_==width&&failedHeight_==height)?failedAt_:SteadyClock::time_point{};
            lock.unlock();
            std::vector<std::uint8_t> idr,jpeg;SteadyClock::time_point at{};bool have=false,failed=false;
            try{have=source_(idr,at)&&!idr.empty()&&idr.size()<=1024*1024&&at!=failedAt;
                const auto now=SteadyClock::now();have=have&&at<=now&&now-at<=maxAge_;
            }catch(...){have=false;}
            if(have){try{jpeg=encode_(idr,quality,width,height);}catch(...){failed=true;}if(jpeg.empty()||jpeg.size()>64000)failed=true;}
            lock.lock();
            if(have)++encodes_;
            if(have&&!failed){jpeg_=std::move(jpeg);quality_=quality;width_=width;height_=height;idrAt_=at;cv_.notify_all();}
            else if(have){failedAt_=at;failedQuality_=quality;failedWidth_=width;failedHeight_=height;} // Never retry the same undecodable IDR.
            // No usable fresh IDR yet: poll briefly until the request budget ends.
            if(!FreshLocked(wantQuality_,wantWidth_,wantHeight_))cv_.wait_for(lock,std::chrono::milliseconds(20),[&]{return stop_;});
        }
    }
    Source source_;
    SizedEncode encode_;
    const std::chrono::milliseconds maxAge_,budget_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    pthread_t thread_{};
    bool started_=false,stop_=false,pending_=false;
    unsigned wantQuality_=0,quality_=0,encodes_=0,wantWidth_=320,wantHeight_=180,width_=0,height_=0;
    unsigned failedQuality_=0,failedWidth_=0,failedHeight_=0;
    SteadyClock::time_point requestUntil_{},idrAt_{},failedAt_{};
    std::vector<std::uint8_t> jpeg_;
};
}
