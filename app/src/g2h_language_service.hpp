#pragma once
#include "g2h_language_catalog.hpp"
#include "g2h_voice_banks.hpp"
#include "g2h_vendor_control.hpp"
#include <condition_variable>
#include <functional>
#include <pthread.h>

namespace g2h {
// One persisted voice-bank pointer controls both native speech and Matter labels.
// The native camera only understands en/cn: both banks use its existing en path.
class LanguageService {
public:
    using Exchange=std::function<bool(const std::string&,std::string&)>;
    struct State{std::optional<language::Id> observed;bool pending=false;const char *error="not-started";};
    explicit LanguageService(Exchange exchange=[](const auto &command,auto &reply){return VendorExchange(command,reply,std::chrono::seconds(1));},
        std::string root="/mnt/sdcard/g2h-matter-camera/voice",
        std::string current="/mnt/config/g2h-matter-camera/voice-current",
        std::string alias="/etc/ch/en"):
        banks_(std::move(root),std::move(current),std::move(alias)),exchange_(std::move(exchange)){}
    ~LanguageService(){Stop();}
    bool Start(){
        if(!banks_.Valid())return false;const auto initial=banks_.Current();if(!initial)return false;
        std::lock_guard<std::mutex> lock(mutex_);if(started_)return false;
        wanted_=*initial;pending_=true;stop_=false;error_="starting";
        pthread_attr_t attr;if(pthread_attr_init(&attr))return false;
        const bool ok=!pthread_attr_setstacksize(&attr,256*1024)&&!pthread_create(&worker_,&attr,Entry,this);
        pthread_attr_destroy(&attr);started_=ok;return ok;
    }
    void Stop(){
        {std::lock_guard<std::mutex> lock(mutex_);if(!started_)return;stop_=true;}
        cv_.notify_all();pthread_join(worker_,nullptr);
        std::lock_guard<std::mutex> lock(mutex_);started_=false;observed_.reset();pending_=false;
    }
    bool Request(language::Id id){
        if(!language::FromMode(static_cast<unsigned>(id)))return false;
        {std::lock_guard<std::mutex> lock(mutex_);if(!started_||stop_)return false;
            wanted_=id;pending_=true;observed_.reset();error_="pending";++generation_;}
        cv_.notify_all();return true;
    }
    State Snapshot()const{
        std::lock_guard<std::mutex> lock(mutex_);State out{observed_,pending_,error_};
        if(out.pending||std::chrono::steady_clock::now()-verified_>=std::chrono::seconds(90))out.observed.reset();
        return out;
    }
private:
    static void *Entry(void *self){static_cast<LanguageService*>(self)->Run();return nullptr;}
    static std::string Trim(std::string text){const auto at=text.find_first_not_of(" \t\r\n");return at==text.npos?"":text.substr(at,text.find_last_not_of(" \t\r\n")-at+1);}
    bool NativeEnglish(){std::string reply;return exchange_("misc language",reply)&&Trim(reply)=="en";}
    bool EnsureNativeEnglish(){
        std::string reply;if(!exchange_("misc language",reply))return false;
        const auto current=Trim(reply);if(current=="en")return true;
        if(current!="cn")return false;
        reply.clear();
        return exchange_("misc language en",reply)&&Trim(reply)=="set misc_language en"&&NativeEnglish();
    }
    const char *Apply(language::Id id){
        if(!banks_.Valid())return "voice-bank-invalid";
        const auto old=banks_.Current();if(!old)return "voice-pointer-invalid";
        if(!EnsureNativeEnglish())return "native-language-unconfirmed";
        if(!banks_.Select(id))return "voice-selection-failed";
        if(!NativeEnglish()||banks_.Current()!=std::optional<language::Id>(id)){
            (void)banks_.Select(*old);return "native-language-unconfirmed";
        }
        return nullptr;
    }
    void Run()noexcept{
        try{
            std::unique_lock<std::mutex> lock(mutex_);
            while(!stop_){
                const bool request=pending_;const auto id=wanted_;const auto generation=generation_;
                lock.unlock();const char *error=nullptr;std::optional<language::Id> current;
                try{
                    if(request){error=Apply(id);if(!error)current=id;}
                    else if(!banks_.Valid())error="voice-bank-invalid";
                    else if(!NativeEnglish())error="native-language-unconfirmed";
                    else{current=banks_.Current();if(!current)error="voice-pointer-invalid";}
                }catch(...){error="language-backend-failed";}
                lock.lock();
                if(generation==generation_){
                    pending_=false;observed_=current;error_=error?error:"none";
                    if(current){verified_=std::chrono::steady_clock::now();(void)language::Publish(*current);}
                }
                cv_.wait_for(lock,std::chrono::seconds(30),[&]{return stop_||pending_;});
            }
        }catch(...){std::lock_guard<std::mutex> lock(mutex_);observed_.reset();pending_=false;error_="language-worker-failed";}
    }
    VoiceBanks banks_;Exchange exchange_;mutable std::mutex mutex_;std::condition_variable cv_;pthread_t worker_{};
    bool started_=false,stop_=false,pending_=false;uint64_t generation_=0;
    language::Id wanted_=language::Id::English;std::optional<language::Id> observed_;
    const char *error_="not-started";std::chrono::steady_clock::time_point verified_{};
};
}
