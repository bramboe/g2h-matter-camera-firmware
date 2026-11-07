#pragma once
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
namespace g2h::media {
using Clock=std::chrono::steady_clock;
inline constexpr std::size_t kMaxParameter=2048,kMaxRecord=65539;
inline void PrivateDirectory(const std::string &path){
 struct stat st{};if(path.empty()||path[0]!='/'||path.size()>80||path.find("..")!=std::string::npos||
 lstat(path.c_str(),&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077))throw std::runtime_error("media directory ownership");
}
inline bool PeerAllowed(int fd){
#if defined(__linux__)
 struct ucred peer{};socklen_t n=sizeof(peer);
 return !getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&n)&&n==sizeof(peer)&&peer.uid==geteuid();
#elif defined(__APPLE__)
 uid_t uid;gid_t gid;return !getpeereid(fd,&uid,&gid)&&uid==geteuid();
#else
 return false;
#endif
}
inline void Wait(int fd,short events,Clock::time_point deadline){
 for(;;){auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();if(ms<=0)throw std::runtime_error("media deadline");
 pollfd p{fd,events,0};int rc=poll(&p,1,int(ms));if(rc<0&&errno==EINTR)continue;
 if(rc<=0||!(p.revents&events))throw std::runtime_error("media disconnected");return;}
}
inline void Transfer(int fd,void *bytes,std::size_t n,bool write,Clock::time_point until){
 auto *p=static_cast<unsigned char*>(bytes);
 while(n){Wait(fd,write?POLLOUT:POLLIN,until);ssize_t done;
 if(write){
#ifdef MSG_NOSIGNAL
 done=send(fd,p,n,MSG_NOSIGNAL);
#else
 done=send(fd,p,n,0);
#endif
 }else done=recv(fd,p,n,0);
 if(done<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;if(done<=0)throw std::runtime_error("media closed");p+=done;n-=std::size_t(done);}
}
inline void Send(int fd,const void *bytes,std::size_t n,std::chrono::milliseconds budget){Transfer(fd,const_cast<void*>(bytes),n,true,Clock::now()+budget);}
inline void Read(int fd,void *bytes,std::size_t n,Clock::time_point until){Transfer(fd,bytes,n,false,until);}
struct Info {
 bool audio=false;unsigned payload=0,channel=0;std::vector<std::uint8_t> sps,pps;
 std::vector<std::uint8_t> Encode()const{
  if(sps.size()>kMaxParameter||pps.size()>kMaxParameter||payload>127||channel>254||
     (audio?(!sps.empty()||!pps.empty()||payload<96):(payload!=36||sps.size()<4||pps.empty()||channel!=0||(sps[0]&159)!=7||(pps[0]&159)!=8)))throw std::runtime_error("media format");
  std::vector<std::uint8_t> b{'G','2','H','M','E','D','0','1',std::uint8_t(audio),std::uint8_t(payload),std::uint8_t(channel),0,
    std::uint8_t(sps.size()>>8),std::uint8_t(sps.size()),std::uint8_t(pps.size()>>8),std::uint8_t(pps.size())};
  b.insert(b.end(),sps.begin(),sps.end());b.insert(b.end(),pps.begin(),pps.end());return b;
 }
 static Info Receive(int fd,bool audio,Clock::time_point until){
  std::uint8_t b[16];Read(fd,b,sizeof(b),until);
  if(std::memcmp(b,"G2HMED01",8)||b[8]!=unsigned(audio)||b[11])throw std::runtime_error("media handshake");
  Info i;i.audio=audio;i.payload=b[9];i.channel=b[10];auto ns=(unsigned(b[12])<<8)|b[13],np=(unsigned(b[14])<<8)|b[15];
  if(ns>kMaxParameter||np>kMaxParameter)throw std::runtime_error("media parameter bounds");
  i.sps.resize(ns);i.pps.resize(np);if(ns)Read(fd,i.sps.data(),ns,until);if(np)Read(fd,i.pps.data(),np,until);(void)i.Encode();return i;
 }
};
}
