#pragma once
#include "g2h_media_protocol.hpp"
#include "g2h_rtsp_source.hpp"
#include <cstdlib>
#include <atomic>
#include <memory>
namespace g2h {
// Closing a subscriber never closes the broker's vendor RTSP session.
class MediaInput {
public:
 H264Parameters parameters;
 explicit MediaInput(bool audio):audio_(audio){}
 ~MediaInput(){Stop();}
 void Start(){
  if(fd_>=0||direct_)throw std::runtime_error("media already started");
  const char *directory=std::getenv("G2H_MEDIA_BROKER_DIR");
  if(!directory||!*directory){
   direct_=std::make_unique<Rtsp>("127.0.0.1",std::chrono::milliseconds(3000),audio_,audio_?"/13":kVideoPath);
   try{if(audio_){auto track=direct_->StartAudio();track_={track.payload,track.channel};}else{direct_->Start();parameters=direct_->parameters;}RenewReadDeadline();return;}catch(...){Stop();throw;}
  }
  media::PrivateDirectory(directory);std::string path=std::string(directory)+(audio_?"/audio.sock":"/video.sock");
  struct stat st{};if(lstat(path.c_str(),&st)||!S_ISSOCK(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077))throw std::runtime_error("media socket ownership");
  sockaddr_un address{};address.sun_family=AF_UNIX;if(path.size()>=sizeof(address.sun_path))throw std::runtime_error("media socket path");std::memcpy(address.sun_path,path.c_str(),path.size()+1);
  fd_=socket(AF_UNIX,SOCK_STREAM,0);if(fd_<0)throw std::runtime_error("media socket");
  try{
   if(fcntl(fd_,F_SETFL,O_NONBLOCK)||fcntl(fd_,F_SETFD,FD_CLOEXEC))throw std::runtime_error("media socket flags");
#ifdef SO_NOSIGPIPE
   int one=1;setsockopt(fd_,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
#endif
   auto until=media::Clock::now()+std::chrono::seconds(4);
   if(connect(fd_,reinterpret_cast<sockaddr*>(&address),sizeof(address))&&errno!=EINPROGRESS)throw std::runtime_error("media broker absent");
   media::Wait(fd_,POLLOUT,until);int error=0;socklen_t n=sizeof(error);
   if(getsockopt(fd_,SOL_SOCKET,SO_ERROR,&error,&n)||error||!media::PeerAllowed(fd_))throw std::runtime_error("media broker peer");
   auto info=media::Info::Receive(fd_,audio_,until);track_={info.payload,info.channel};parameters.sps=std::move(info.sps);parameters.pps=std::move(info.pps);RenewReadDeadline();
  }catch(...){Stop();throw;}
 }
 struct Track {unsigned payload=0,channel=0;};
 Track AudioTrack()const{return track_;}
 void Stop()noexcept{if(fd_>=0){close(fd_);fd_=-1;}direct_.reset();track_={};parameters.sps.clear();parameters.pps.clear();}
 void RenewReadDeadline(){if(direct_)direct_->RenewReadDeadline();until_=media::Clock::now()+std::chrono::seconds(2);}
 void Read(void *data,std::size_t count){if(direct_)direct_->Read(data,count);else if(fd_>=0)media::Read(fd_,data,count,until_);else throw std::runtime_error("media source closed");}
 // An intentional capture pause may leave a subscriber idle or midway through
 // a record. Exclude only explicitly paused time from the existing
 // record budget. Framing, EOF/HUP and unpaused deadlines remain strict; the
 // caller's stop flag is checked every 100 ms, including during a long pause.
 // This does not establish native quiescence or flush any upstream buffer.
 template<class PauseClock>
 void ReadWithCapturePause(void *data,std::size_t count,const std::atomic<bool>&allowed,const std::atomic<bool>&cancel,PauseClock pausedTime,std::chrono::nanoseconds pauseAtRecordStart){
  const int fd=direct_?direct_->Descriptor():fd_;
  if(fd<0)throw std::runtime_error("media source closed");
  auto *out=static_cast<std::uint8_t*>(data);
  while(count){
   if(cancel)throw std::runtime_error("media read cancelled");
   const bool paused=!allowed.load();const auto before=media::Clock::now();
   const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(until_+pausedTime()-pauseAtRecordStart-before).count();
   if(!paused&&left<=0)throw std::runtime_error("media deadline");
   pollfd descriptor{fd,POLLIN,0};
   const int wait=paused?100:static_cast<int>(std::min<std::int64_t>(100,std::max<std::int64_t>(1,left)));
   const int ready=poll(&descriptor,1,wait);
   if(cancel)throw std::runtime_error("media read cancelled");
   if(ready<0&&errno==EINTR)continue;
   if(ready<0||(descriptor.revents&(POLLERR|POLLHUP|POLLNVAL)))throw std::runtime_error("media transport unavailable");
   if(!ready)continue;
   if(!(descriptor.revents&POLLIN))throw std::runtime_error("media transport unavailable");
   const auto n=recv(fd,out,count,0);
   if(n<0&&(errno==EINTR||errno==EAGAIN))continue;
   if(n<=0)throw std::runtime_error("media receive");
   out+=n;count-=static_cast<std::size_t>(n);
  }
 }

private:
 bool audio_;int fd_=-1;std::unique_ptr<Rtsp> direct_;Track track_{};
 media::Clock::time_point until_=media::Clock::now()+std::chrono::seconds(2);
};
}
