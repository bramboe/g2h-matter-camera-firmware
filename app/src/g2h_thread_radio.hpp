#pragma once
// Serialized diagnostics and explicit enable control for the standalone Thread router.
// No dataset/key requests, reset, GPIO operations, commissioning or flash access.
#include "g2h_thread_telemetry.hpp"
#include <atomic>
#include <cerrno>
#ifdef G2H_RADIO_TRACE
#include <cstdio>
#endif
#include <chrono>
#include <condition_variable>
#include <optional>
#include <cstdint>
#include <string>
#include <vector>
#include <sstream>
#include <mutex>
#include <thread>
#include <poll.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>

namespace g2h {
struct RadioSnapshot {
    bool valid=false;
    std::string role,network,extAddress,extendedPanId;
    unsigned channel=0,panId=0,rloc16=0;
    std::optional<bool> enabled; // Only known after matching state + ifconfig replies.
    RadioDataset neighbors,children,routers; RadioCounters counters;
    std::chrono::steady_clock::time_point sampled{};
    bool Fresh() const {return valid&&std::chrono::steady_clock::now()-sampled<std::chrono::seconds(90);}
};
struct ThreadControlSnapshot {
    std::optional<bool> desired,observed;
    bool pending=false;
    std::string error; // Bounded local category, never CLI text or network credentials.
    uint64_t generation=0;
};
class ThreadRadio {
public:
    explicit ThreadRadio(std::string uart="/dev/ttyS2",std::string lock="/tmp/g2h_thread_status.lock"):
        uart_(std::move(uart)),lock_(std::move(lock)){}
    ~ThreadRadio(){Stop();}
    void Start(){if(worker_.joinable())return;stop_=false;worker_=std::thread([this]{
        while(!stop_){PollOnce();std::unique_lock<std::mutex> guard(mutex_);
            cv_.wait_for(guard,std::chrono::seconds(30),[this]{return stop_||control_.pending;});}
    });}
    void Stop(){stop_=true;cv_.notify_all();if(worker_.joinable())worker_.join();}
    // Accepted means queued, not applied. No default startup write. The owner may
    // restore an explicitly persisted preference by calling this same API.
    bool SetEnabled(bool enabled){
        {std::lock_guard<std::mutex> guard(mutex_);control_.desired=enabled;
            control_.pending=true;control_.error.clear();++control_.generation;}
        cv_.notify_all();return true;
    }
    ThreadControlSnapshot ControlSnapshot() const {
        std::lock_guard<std::mutex> guard(mutex_);auto out=control_;
        if(std::chrono::steady_clock::now()-observedAt_>=std::chrono::seconds(90))out.observed.reset();return out;
    }
    RadioSnapshot Snapshot() const {std::lock_guard<std::mutex> guard(mutex_);return snapshot_;}
    bool PollOnce(){
        // One in-process owner plus the existing cross-process maintenance lock.
        std::lock_guard<std::mutex> ioGuard(ioMutex_);
        if(mkdir(lock_.c_str(),0700))return Invalidate("uart-busy");
        struct Lock {std::string path;~Lock(){rmdir(path.c_str());}} lock{lock_};
        int fd=open(uart_.c_str(),O_RDWR|O_NOCTTY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0){
#ifdef G2H_RADIO_TRACE
std::fprintf(stderr,"radio open errno=%d\n",errno);
#endif
return Invalidate("uart-unavailable");}
        struct Serial {int fd;termios old{};bool saved=false;~Serial(){if(saved)tcsetattr(fd,TCSANOW,&old);close(fd);}} serial{fd};
        if(tcgetattr(fd,&serial.old))return Invalidate("uart-settings");serial.saved=true;
        termios raw=serial.old;cfmakeraw(&raw);raw.c_cflag|=CLOCAL|CREAD;raw.c_cflag&=~(HUPCL|CRTSCTS);raw.c_cc[VMIN]=0;raw.c_cc[VTIME]=0;
        if(cfsetispeed(&raw,B115200)||cfsetospeed(&raw,B115200)||tcsetattr(fd,TCSANOW,&raw))return Invalidate("uart-settings");
        // Writes stay on this same worker/serial lock. An unsuccessful attempt is
        // reported and never blindly replayed every poll. A later explicit write retries.
        ThreadControlSnapshot attempt;{std::lock_guard<std::mutex> guard(mutex_);attempt=control_;}
        if(attempt.pending){
            const bool enabled=*attempt.desired;
            const char *first=enabled?"ifconfig up":"thread stop";
            const char *second=enabled?"thread start":"ifconfig down";
            std::vector<std::string> reply;
            if(!QueryLines(fd,first,reply)||!reply.empty()||
               !QueryLines(fd,second,reply)||!reply.empty()){
                FinishControl(attempt.generation,"command-failed");return Invalidate();
            }
            std::string role,iface;
            if(!Query(fd,"state",role)||!Query(fd,"ifconfig",iface)||
               !EnabledState(role,iface).has_value()||*EnabledState(role,iface)!=enabled){
                FinishControl(attempt.generation,"verification-failed");return Invalidate();
            }
            FinishControl(attempt.generation,"",enabled);
        }
        // CLI responses can contain old asynchronous lines; correlate using the
        // echoed read-only command and then require an exact Done terminator.
        RadioSnapshot next;std::string value;
        if(!Query(fd,"state",value)||!Role(value))return Invalidate();next.role=value;
        if(Query(fd,"ifconfig",value))next.enabled=EnabledState(next.role,value);
        Observe(next.enabled);
        if(!Query(fd,"channel",value)||!Number(value,10,26,next.channel)||next.channel<11)return Invalidate();
        if(!Query(fd,"networkname",value)||value.empty()||value.size()>16)return Invalidate();next.network=value;
        if(!Query(fd,"panid",value)||!Number(value,16,65535,next.panId))return Invalidate();
        if(!Query(fd,"extpanid",value)||!Hex(value,16))return Invalidate();next.extendedPanId=value;
        if(!Query(fd,"extaddr",value)||!Hex(value,16))return Invalidate();next.extAddress=value;
        if(!Query(fd,"rloc16",value)||!Number(value,16,65535,next.rloc16))return Invalidate();
        // Optional read-only commands. Unsupported/malformed output invalidates only
        // its own dataset, never the identity/role sample or another table.
        std::vector<std::string> rows;
        if(QueryLines(fd,"neighbor table",rows))ParseRadioTable(rows,"neighbor",next.neighbors);
        if(QueryLines(fd,"child table",rows))ParseRadioTable(rows,"child",next.children);
        if(QueryLines(fd,"router table",rows))ParseRadioTable(rows,"router",next.routers);
        std::vector<std::string> counters;
        if(QueryLines(fd,"counters mac",counters)&&Query(fd,"uptime ms",value))ParseRadioCounters(counters,value,next.counters);
        next.valid=true;next.sampled=std::chrono::steady_clock::now();
        {std::lock_guard<std::mutex> guard(mutex_);snapshot_=std::move(next);}return true;
    }
    static bool Number(std::string text,unsigned base,unsigned maximum,unsigned &out){
        if(base==16&&text.size()>2&&text.substr(0,2)=="0x")text.erase(0,2);
        if(text.empty()||text.size()>10)return false;unsigned result=0;
        for(char c:text){unsigned d=c>='0'&&c<='9'?unsigned(c-'0'):c>='a'&&c<='f'?unsigned(c-'a'+10):c>='A'&&c<='F'?unsigned(c-'A'+10):99;
            if(d>=base||d>maximum||result>(maximum-d)/base)return false;result=result*base+d;}
        out=result;return true;
    }
private:
    void Observe(std::optional<bool> value){
        std::lock_guard<std::mutex> guard(mutex_);control_.observed=value;observedAt_=std::chrono::steady_clock::now();
    }
    void FinishControl(uint64_t generation,const char *error,std::optional<bool> observed=std::nullopt){
        std::lock_guard<std::mutex> guard(mutex_);control_.observed=observed;observedAt_=std::chrono::steady_clock::now();
        if(control_.generation==generation){control_.pending=false;control_.error=error;}
    }
    bool Invalidate(const char *controlError=""){
        std::lock_guard<std::mutex> guard(mutex_);snapshot_.valid=false;control_.observed.reset();
        if(control_.pending&&*controlError){control_.pending=false;control_.error=controlError;}
        return false;
    }
    static std::optional<bool> EnabledState(const std::string &role,const std::string &iface){
        if(role=="disabled"&&iface=="down")return false;
        if((role=="detached"||role=="child"||role=="router"||role=="leader")&&iface=="up")return true;
        return std::nullopt;
    }
    static bool Role(const std::string &s){return s=="disabled"||s=="detached"||s=="child"||s=="router"||s=="leader";}
    static bool Hex(const std::string &s,size_t n){if(s.size()!=n)return false;for(char c:s)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return false;return true;}
    bool Query(int fd,const char *command,std::string &value){
        std::vector<std::string> lines;if(!QueryLines(fd,command,lines)||lines.size()!=1)return false;value=lines.front();return true;
    }
    bool QueryLines(int fd,const char *command,std::vector<std::string> &value){
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        std::string request=std::string(command)+"\r\n",response;size_t sent=0,total=0;bool echoed=false;std::vector<std::string> lines;
        while(!stop_&&std::chrono::steady_clock::now()<deadline){
            pollfd p{fd,short(sent<request.size()?POLLOUT:POLLIN),0};int rc=poll(&p,1,100);
            if(rc<0){if(errno==EINTR)continue;return false;}if(!rc)continue;
            if(p.revents&(POLLERR|POLLHUP|POLLNVAL))return false;
            if(sent<request.size()){
                auto n=write(fd,request.data()+sent,request.size()-sent);if(n>0)sent+=size_t(n);else if(n<0&&errno!=EAGAIN&&errno!=EINTR)return false;continue;
            }
            char buf[256];auto n=read(fd,buf,sizeof(buf));if(n<0){if(errno==EAGAIN||errno==EINTR)continue;return false;}
            if(!n)continue;
#ifdef G2H_RADIO_TRACE
std::fprintf(stderr,"radio %s: %.*s",command,int(n),buf);
#endif
total+=size_t(n);if(total>16384)return false;response.append(buf,size_t(n));if(response.size()>1024)return false;
            size_t end;
            while((end=response.find('\n'))!=std::string::npos){
                std::string line=response.substr(0,end);response.erase(0,end+1);
                if(!line.empty()&&line.back()=='\r')line.pop_back();
                while(line.rfind("> ",0)==0)line.erase(0,2);
                std::string echo=line;while(!echo.empty()&&(echo.front()==' '||echo.front()=='>'||echo.front()=='\t'))echo.erase(0,1);
                if(echo==command){echoed=true;lines.clear();continue;}
                if(!echoed||line.empty()||line==">")continue;
                if(line=="Done"){value=std::move(lines);return true;}
                if(line.rfind("Error",0)==0)return false;lines.push_back(line);if(lines.size()>66)return false;
            }
        }return false;
    }
    std::string uart_,lock_;
    mutable std::mutex mutex_;std::mutex ioMutex_;std::condition_variable cv_;
    RadioSnapshot snapshot_;ThreadControlSnapshot control_;
    std::chrono::steady_clock::time_point observedAt_{};
    std::atomic<bool> stop_{false};std::thread worker_;
};
}
