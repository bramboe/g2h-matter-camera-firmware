#pragma once
#include "g2h_media_protocol.hpp"
#include "g2h_rtsp_source.hpp"
#include <cstdlib>
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
private:
 bool audio_;int fd_=-1;std::unique_ptr<Rtsp> direct_;Track track_{};
 media::Clock::time_point until_=media::Clock::now()+std::chrono::seconds(2);
};
}
