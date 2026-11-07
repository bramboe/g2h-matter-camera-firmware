#pragma once
#include "g2h_motion_observer.hpp"
#include <pthread.h>
#include <condition_variable>
#include <mutex>
namespace g2h {
// Separate from the Matter event loop and media workers. Read-only, one Hz.
class MotionSource {
public:
    MotionState state;
    ~MotionSource(){Stop();}
    bool Start(){
        std::lock_guard<std::mutex> lock(mutex_);if(started_)return true;
        stop_=false;pthread_attr_t attr;if(pthread_attr_init(&attr))return false;
        bool ok=!pthread_attr_setstacksize(&attr,512*1024)&&!pthread_create(&thread_,&attr,Entry,this);
        pthread_attr_destroy(&attr);started_=ok;return ok;
    }
    void Stop(){
        {std::lock_guard<std::mutex> lock(mutex_);if(!started_)return;stop_=true;}
        cv_.notify_all();pthread_join(thread_,nullptr);
        std::lock_guard<std::mutex> lock(mutex_);started_=false;state.Set({{},0,"stopped",{}});
    }
private:
    static void *Entry(void *p){static_cast<MotionSource*>(p)->Run();return nullptr;}
    void Run()noexcept{
        MotionObserver observer;std::unique_lock<std::mutex> lock(mutex_);
        while(!stop_){
            lock.unlock();
            try{auto pid=observer.FindVendorPid();state.Set(pid?observer.Observe(*pid):MotionObservation{{},0,"process-unavailable",{}});}
            catch(...){state.Set({{},0,"observer-failed",{}});}
            lock.lock();cv_.wait_for(lock,std::chrono::seconds(1),[this]{return stop_;});
        }
    }
    std::mutex mutex_;std::condition_variable cv_;pthread_t thread_{};bool started_=false,stop_=false;
};
}
