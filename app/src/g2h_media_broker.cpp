// Owns native RTSP sessions independently of the Matter app. No video/audio is
// saved: complete interleaved records are drained or sent to one local consumer.
#include "g2h_media_protocol.hpp"
#include "g2h_rtsp_source.hpp"
#include <atomic>
#include <csignal>
#include <cstdio>
#include <memory>
#include <pthread.h>
#include <sys/file.h>
namespace {
std::atomic<bool> stop{false};
static_assert(std::atomic<bool>::is_always_lock_free,"Signal flag must be lock-free");
using namespace g2h::media;
class Stream {
public:
 Stream(std::string directory,bool audio,std::uint16_t port):directory_(std::move(directory)),audio_(audio),port_(port){}
 ~Stream(){if(threaded_)pthread_join(thread_,nullptr);CloseClient();if(listener_>=0)close(listener_);if(bound_)unlink(Path().c_str());}
 bool Start(){
  path_=Path();struct stat st{};
  if(!lstat(path_.c_str(),&st)){
   if(!S_ISSOCK(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077)||unlink(path_.c_str()))return false;
  }else if(errno!=ENOENT)return false;
  listener_=socket(AF_UNIX,SOCK_STREAM,0);if(listener_<0)return false;
  if(fcntl(listener_,F_SETFL,O_NONBLOCK)||fcntl(listener_,F_SETFD,FD_CLOEXEC))return false;
  sockaddr_un address{};address.sun_family=AF_UNIX;if(path_.size()>=sizeof(address.sun_path))return false;
  std::memcpy(address.sun_path,path_.c_str(),path_.size()+1);
  if(bind(listener_,reinterpret_cast<sockaddr*>(&address),sizeof(address)))return false;bound_=true;
  if(chmod(path_.c_str(),0600)||listen(listener_,2))return false;
  pthread_attr_t attr;if(pthread_attr_init(&attr))return false;
  bool ok=!pthread_attr_setstacksize(&attr,256*1024)&&!pthread_create(&thread_,&attr,Entry,this);pthread_attr_destroy(&attr);threaded_=ok;return ok;
 }
private:
 std::string Path()const{return directory_+(audio_?"/audio.sock":"/video.sock");}
 static void *Entry(void *p){static_cast<Stream*>(p)->Run();return nullptr;}
 void CloseClient(){if(client_>=0){close(client_);client_=-1;}}
 void Accept(){
  for(unsigned i=0;i<2;++i){
   int next=accept(listener_,nullptr,nullptr);if(next<0)return;
   if(client_>=0){pollfd p{client_,POLLIN,0};if(poll(&p,1,0)>0){char c;auto n=recv(client_,&c,1,MSG_PEEK|MSG_DONTWAIT);if(n<=0)CloseClient();}}
   if(client_>=0||fault_||Clock::now()<connectRetryAt_||!PeerAllowed(next)||fcntl(next,F_SETFL,O_NONBLOCK)||fcntl(next,F_SETFD,FD_CLOEXEC)){close(next);continue;}
#ifdef SO_NOSIGPIPE
   int one=1;setsockopt(next,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
#endif
   // AAC decoder startup can briefly pause its reader. Keep a bounded audio
   // cushion without letting unrelated video consume it.
   int sendBuffer=audio_?256*1024:65536;setsockopt(next,SOL_SOCKET,SO_SNDBUF,&sendBuffer,sizeof(sendBuffer));
   client_=next;
   if(!source_){
    try{
     source_=std::make_unique<Rtsp>("127.0.0.1",std::chrono::milliseconds(3000),audio_,audio_?"/13":g2h::kVideoPath,port_);
     info_.audio=audio_;
     if(audio_){auto track=source_->StartAudio();info_.payload=track.payload;info_.channel=track.channel;}
     else{source_->Start();info_.payload=36;info_.channel=0;info_.sps=source_->parameters.sps;info_.pps=source_->parameters.pps;}
    }catch(...){
     CloseClient();
     if(!source_){
      // The socket constructor failed before any RTSP command was sent. Boot
      // may expose our local listener before the native server starts listening.
      // Retry only this safe unopened state, with a bounded connection rate.
      connectRetryAt_=Clock::now()+std::chrono::seconds(5);
      std::fputs(audio_?"media audio connect deferred\n":"media video connect deferred\n",stderr);
     }else{
      // A DESCRIBE/SETUP/PLAY attempt may have native state: retain its ownership
      // and require the established full-boot recovery rather than reopening it.
      fault_=true;std::fputs(audio_?"media audio source fault\n":"media video source fault\n",stderr);
     }
     continue;
    }
   }
   // An abandoning/slow subscriber is not a source fault. Keep draining the
   // already-open native session and permit the next subscriber to reconnect.
   try{auto bytes=info_.Encode();Send(client_,bytes.data(),bytes.size(),std::chrono::milliseconds(100));}
   catch(...){CloseClient();}
  }
 }
 void Run()noexcept{
  try{
   std::vector<std::uint8_t> record;
   while(!stop){
    Accept();
    if(!source_||fault_){pollfd p{listener_,POLLIN,0};poll(&p,1,100);continue;}
    pollfd p{source_->Descriptor(),POLLIN,0};int ready=poll(&p,1,100);
    if(ready<0&&errno==EINTR)continue;
    if(ready<0||(p.revents&(POLLERR|POLLHUP|POLLNVAL)))throw std::runtime_error("native media closed");
    if(!ready)continue; // Idle hardware privacy must not close a native session.
    std::uint8_t header[4];source_->RenewReadDeadline();source_->Read(header,sizeof(header));
    std::size_t size=(unsigned(header[2])<<8)|header[3];
    if(header[0]!='$'||!size||size+4>kMaxRecord)throw std::runtime_error("native media record");
    record.assign(header,header+4);record.resize(size+4);source_->Read(record.data()+4,size);
    // This vendor sends video on the audio-only session as well. Drain those
    // records without forwarding them: a keyframe otherwise fills the audio
    // subscriber's socket while its first AAC frame is being decoded.
    if(audio_&&header[1]!=info_.channel&&header[1]!=info_.channel+1)continue;
    if(client_>=0){try{Send(client_,record.data(),record.size(),std::chrono::milliseconds(audio_?250:25));}catch(...){CloseClient();}}
   }
  }catch(...){fault_=true;CloseClient();std::fputs(audio_?"media audio ingest stopped\n":"media video ingest stopped\n",stderr);}
  // Continue rejecting subscribers promptly after a native ingest fault; leaving
  // only the kernel backlog alive would make every reconnect wait for a timeout.
  while(!stop){Accept();pollfd p{listener_,POLLIN,0};poll(&p,1,100);}
  // Native session remains owned until broker shutdown, even after a subscriber
  // faults. A Matter restart never destructs this object or sends TEARDOWN.
 }
 const std::string directory_;std::string path_;bool audio_,threaded_=false,bound_=false,fault_=false;
 std::uint16_t port_;int listener_=-1,client_=-1;pthread_t thread_{};
 std::unique_ptr<Rtsp> source_;Info info_;Clock::time_point connectRetryAt_{};
};
}
int main(int argc,char **argv){
 if(argc!=3||std::strcmp(argv[1],"--serve")){std::fputs("Use --serve PRIVATE_SOCKET_DIRECTORY\n",stderr);return 2;}
 std::signal(SIGPIPE,SIG_IGN);std::signal(SIGTERM,[](int){stop=true;});std::signal(SIGINT,[](int){stop=true;});
 try{
  std::string directory=argv[2];if(mkdir(directory.c_str(),0700)&&errno!=EEXIST)return 2;PrivateDirectory(directory);
  int lock=open((directory+"/lock").c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
  if(lock<0||flock(lock,LOCK_EX|LOCK_NB))return 2;
  std::uint16_t port=554;
#ifdef G2H_BROKER_TESTING
  if(const char *text=std::getenv("G2H_TEST_RTSP_PORT")){long value=std::strtol(text,nullptr,10);if(value<1024||value>65535)return 2;port=std::uint16_t(value);}
#endif
  Stream video(directory,false,port),audio(directory,true,port);
  if(!video.Start()||!audio.Start()){stop=true;return 1;}
  std::puts("G2H_MEDIA_BROKER_READY");std::fflush(stdout);
  while(!stop)poll(nullptr,0,100);
  return 0;
 }catch(...){stop=true;return 1;}
}
