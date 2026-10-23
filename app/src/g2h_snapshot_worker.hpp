#pragma once
#include <pthread.h>
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
    using Encode=std::function<std::vector<std::uint8_t>(const std::vector<std::uint8_t>&,unsigned)>;
    SnapshotWorker(Source source,Encode encode,std::chrono::milliseconds maxAge=std::chrono::seconds(3),
                   std::chrono::milliseconds requestBudget=std::chrono::milliseconds(2500))
        :source_(std::move(source)),encode_(std::move(encode)),maxAge_(maxAge),budget_(requestBudget){}
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
        std::lock_guard<std::mutex> lock(mutex_);started_=false;pending_=false;jpeg_.clear();
    }
    // Non-blocking: begin preparing a JPEG at this quality.
    void Request(unsigned quality){std::lock_guard<std::mutex> lock(mutex_);if(started_&&!stop_)RequestLocked(quality);}
    bool Get(unsigned quality,std::chrono::milliseconds wait,std::vector<std::uint8_t>&out){
        std::unique_lock<std::mutex> lock(mutex_);
        if(!started_||stop_)return false;
        if(!FreshLocked(quality)){
            RequestLocked(quality);
            cv_.wait_for(lock,wait,[&]{return stop_||FreshLocked(quality);});
            if(stop_||!FreshLocked(quality))return false;
        }
        out=jpeg_;return true;
    }
    unsigned Encodes() const {std::lock_guard<std::mutex> lock(mutex_);return encodes_;}
private:
    static void *Entry(void *self){try{static_cast<SnapshotWorker*>(self)->Run();}catch(...){}return nullptr;}
    bool FreshLocked(unsigned quality) const {
        return !jpeg_.empty()&&quality_==quality&&SteadyClock::now()-idrAt_<=maxAge_;
    }
    void RequestLocked(unsigned quality){
        const auto until=SteadyClock::now()+budget_;
        if(!pending_||wantQuality_!=quality||until>requestUntil_)requestUntil_=until;
        wantQuality_=quality;pending_=true;cv_.notify_all();
    }
    void Run(){
        std::unique_lock<std::mutex> lock(mutex_);
        while(!stop_){
            cv_.wait(lock,[&]{return stop_||pending_;});
            if(stop_)break;
            const unsigned quality=wantQuality_;
            if(FreshLocked(quality)||SteadyClock::now()>=requestUntil_){pending_=false;cv_.notify_all();continue;}
            const auto failedAt=failedAt_;
            lock.unlock();
            std::vector<std::uint8_t> idr,jpeg;SteadyClock::time_point at{};bool have=false,failed=false;
            try{have=source_(idr,at)&&!idr.empty()&&at!=failedAt;}catch(...){have=false;}
            if(have){try{jpeg=encode_(idr,quality);}catch(...){failed=true;}if(jpeg.empty())failed=true;}
            lock.lock();
            if(have)++encodes_;
            if(have&&!failed){jpeg_=std::move(jpeg);quality_=quality;idrAt_=at;cv_.notify_all();}
            else if(have)failedAt_=at; // Never retry the same undecodable IDR.
            // No usable fresh IDR yet: poll briefly until the request budget ends.
            if(!FreshLocked(wantQuality_))cv_.wait_for(lock,std::chrono::milliseconds(20),[&]{return stop_;});
        }
    }
    Source source_;
    Encode encode_;
    const std::chrono::milliseconds maxAge_,budget_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    pthread_t thread_{};
    bool started_=false,stop_=false,pending_=false;
    unsigned wantQuality_=0,quality_=0,encodes_=0;
    SteadyClock::time_point requestUntil_{},idrAt_{},failedAt_{};
    std::vector<std::uint8_t> jpeg_;
};
}
