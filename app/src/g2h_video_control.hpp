#pragma once
#include <atomic>
#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace g2h {
// A capture state is observed from the preference AND kernel counters; a queued
// reboot never becomes an optimistic state. This backend is optional on older
// installations and must be installed separately with a verified native profile.
class VideoControl {
public:
 explicit VideoControl(std::string config="/mnt/config/g2h-matter-camera",
     std::string helper="/customer/g2h-matter-camera/g2h-video-control",
     std::string proc="/proc",std::string pause="/tmp/g2h-capture-control"):
     config_(std::move(config)),helper_(std::move(helper)),proc_(std::move(proc)),pause_(std::move(pause)){}
 ~VideoControl(){if(worker_.joinable())worker_.join();}
 bool Available()const{return Private(config_+"/video-control-installed")==std::optional<std::string>("g2h-video-boot-v1\n");}
 std::optional<bool> Observe(){
  if(pending_)return std::nullopt;
  const auto now=std::chrono::steady_clock::now();
  if(now<nextSample_)return observed_;
  nextSample_=now+std::chrono::seconds(1);observed_.reset();
  if(!Available())return observed_;
  auto preference=Private(config_+"/video-enabled");
  bool enabled=true;
  if(preference){if(*preference=="0\n")enabled=false;else if(*preference!="1\n")return observed_;}
  else{struct stat st{};if(!lstat((config_+"/video-enabled").c_str(),&st)||errno!=ENOENT)return observed_;}
  auto counter=Number(proc_+"/mstar/ISP/DevInfo/FrameNumber");if(!counter)return observed_;
  bool off=*counter==0,on=previousCounter_&&*counter>*previousCounter_;
  previousCounter_=counter;
  for(unsigned channel=0;channel<2;++channel){
   const auto id=std::to_string(channel);
   for(const auto &path:{"/mstar/MI/VENC/VeChn"+id+"/Create","/mstar/MI/VENC/VeChn"+id+"/Start","/mstar/MI/VI/ViChn"+id+"/Enable"}){
    auto value=Number(proc_+path);off=off&&value&&*value==0;
    // This exact camera profile uses channel 0. Channel 1 is normally
    // uncreated; requiring it to run would make the real ON state unknown.
    on=on&&value&&*value<=1&&(channel!=0||*value==1);
   }
  }
  if(!enabled&&off&&Private(pause_+"/paused").has_value())observed_=false;
  if(enabled&&on)observed_=true;
  return observed_;
 }
 bool Request(bool enabled){
  if(!Available()||pending_)return false;
  if(worker_.joinable())worker_.join();
  pending_=true;
  try{worker_=std::thread([this,enabled]{
   const pid_t child=fork();
   if(child==0){execl(helper_.c_str(),helper_.c_str(),"set",enabled?"1":"0",static_cast<char*>(nullptr));_exit(127);}
   int status=0;pid_t waited=-1;
   if(child>0){do{waited=waitpid(child,&status,0);}while(waited<0&&errno==EINTR);}
   // Success keeps pending until the full boot constructs a new app instance.
   // Failure permits a retry and never reports the requested state as observed.
   if(child<0||waited!=child||!WIFEXITED(status)||WEXITSTATUS(status)!=0)pending_=false;
  });return true;}catch(...){pending_=false;return false;}
 }
private:
 static std::optional<std::string> Read(const std::string &path,bool privateFile){
  int fd=open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return {};
  struct stat st{};
  if(fstat(fd,&st)||!S_ISREG(st.st_mode)||(privateFile&&(st.st_uid!=geteuid()||(st.st_mode&077)||st.st_nlink!=1))){close(fd);return {};}
  char bytes[65];const auto n=read(fd,bytes,sizeof(bytes));close(fd);if(n<0||n>=static_cast<ssize_t>(sizeof(bytes)))return {};
  return std::string(bytes,static_cast<std::size_t>(n));
 }
 static std::optional<std::string> Private(const std::string &path){return Read(path,true);}
 static std::optional<unsigned long long> Number(const std::string &path){
  auto text=Read(path,false);if(!text||text->empty())return {};
  unsigned long long value=0;std::size_t at=0;
  for(;at<text->size()&&(*text)[at]>='0'&&(*text)[at]<='9';++at){if(value>1000000000000000000ULL)return {};value=value*10+unsigned((*text)[at]-'0');}
  if(!at)return {};for(;at<text->size();++at)if((*text)[at]!='\n'&&(*text)[at]!=' '&&(*text)[at]!='\t')return {};
  return value;
 }
 std::string config_,helper_,proc_,pause_;std::atomic<bool> pending_{false};std::thread worker_;
 std::chrono::steady_clock::time_point nextSample_{};std::optional<bool> observed_;
 std::optional<unsigned long long> previousCounter_;
};
}
