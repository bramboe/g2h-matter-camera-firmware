#pragma once
#include "g2h_media_protocol.hpp"
#include <algorithm>
#include <atomic>
namespace g2h::media {
// A root-owned local marker is an explicit pause request, NOT a measurement of
// sensor state. Untrusted marker state also suppresses output while retaining
// the native session; it must never terminate the broker. Keep the retained native TCP session alive across partial reads.
class CapturePause {
public:
 explicit CapturePause(std::string directory="/tmp/g2h-capture-control"):
  directory_(std::move(directory)),sampled_(Clock::now()){}
 bool Paused(){Update();return paused_;}
 void BeginRecord(){Update();until_=Clock::now()+std::chrono::seconds(2);baseline_=elapsed_;recordEpoch_=epoch_;}
 bool DiscardRecord(){Update();return paused_||recordEpoch_!=epoch_;}
 void Read(int fd,void *bytes,std::size_t count,const std::atomic<bool>&cancel){
  auto *out=static_cast<unsigned char*>(bytes);
  while(count){
   if(cancel)throw std::runtime_error("capture read cancelled");
   Update();const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(until_+elapsed_-baseline_-Clock::now()).count();
   if(!paused_&&left<=0)throw std::runtime_error("native media deadline");
   pollfd p{fd,POLLIN,0};const int wait=paused_?100:int(std::clamp<std::int64_t>(left,1,100));
   const int ready=poll(&p,1,wait);
   if(cancel)throw std::runtime_error("capture read cancelled");
   if(ready<0&&errno==EINTR)continue;
   if(ready<0||(p.revents&(POLLERR|POLLHUP|POLLNVAL)))throw std::runtime_error("native media closed");
   if(!ready)continue;
   if(!(p.revents&POLLIN))throw std::runtime_error("native media unavailable");
   const auto n=recv(fd,out,count,0);
   if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
   if(n<=0)throw std::runtime_error("native media closed");out+=n;count-=std::size_t(n);
  }
 }
private:
 bool Marker(){
  struct stat directory{};
  if(lstat(directory_.c_str(),&directory)){return errno!=ENOENT;}
  if(!S_ISDIR(directory.st_mode)||directory.st_uid!=geteuid()||(directory.st_mode&077))return true;
  struct stat marker{};const auto path=directory_+"/paused";
  if(lstat(path.c_str(),&marker)){return errno!=ENOENT;}
  if(!S_ISREG(marker.st_mode)||marker.st_uid!=geteuid()||(marker.st_mode&077)||marker.st_nlink!=1)return true;
  return true;
 }
 void Update(){
  const auto now=Clock::now();if(paused_)elapsed_+=now-sampled_;sampled_=now;
  const bool next=Marker();if(next!=paused_){paused_=next;++epoch_;}
 }
 std::string directory_;bool paused_=false;std::uint64_t epoch_=0,recordEpoch_=0;
 Clock::time_point sampled_,until_{};Clock::duration elapsed_{},baseline_{};
};
}
