#define G2H_TALKBACK_TEST_HOOKS
#include "g2h_talkback.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <thread>
using namespace std::chrono_literals;
struct Gate {
 std::mutex mutex;std::condition_variable cv;bool reached=false,released=false;
 void Block(){std::unique_lock<std::mutex> l(mutex);reached=true;cv.notify_all();assert(cv.wait_for(l,5s,[&]{return released;}));}
 void Await(){std::unique_lock<std::mutex> l(mutex);assert(cv.wait_for(l,5s,[&]{return reached;}));}
 void Release(){std::lock_guard<std::mutex> l(mutex);released=true;cv.notify_all();}
};
template<class F>void Until(F f){const auto end=std::chrono::steady_clock::now()+5s;while(!f()){assert(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(2ms);}}
struct FakeVendor {
 int fd;std::atomic<bool> stop{false};std::atomic<unsigned> calls{0},invalid{0};std::thread thread;
 FakeVendor(){fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof a)==0);socklen_t n=sizeof a;assert(getsockname(fd,reinterpret_cast<sockaddr*>(&a),&n)==0);g2h::vendorDebugPort=ntohs(a.sin_port);assert(listen(fd,8)==0);
 thread=std::thread([&]{while(!stop){pollfd p{fd,POLLIN,0};if(poll(&p,1,20)<=0)continue;int c=accept(fd,nullptr,nullptr);assert(c>=0);std::string command;char b[512];ssize_t n;while((n=recv(c,b,sizeof b,0))>0)command.append(b,std::size_t(n));
 if(!command.empty()&&command.back()==0)command.pop_back();const std::string prefix="function debug_playfile ";
 if(command.rfind(prefix,0)!=0)++invalid;else{FILE *file=fopen(command.substr(prefix.size()).c_str(),"rb");std::vector<std::uint8_t> bytes;if(file){int b;while((b=fgetc(file))!=EOF)bytes.push_back(std::uint8_t(b));fclose(file);}if(!g2h::ValidAdts(bytes))++invalid;++calls;}
 close(c);}});}
 ~FakeVendor(){stop=true;thread.join();close(fd);}
};
struct Directory{std::string path;Directory(){char p[]="/tmp/g2h-talk-mute-XXXXXX";assert(mkdtemp(p));path=p;}~Directory(){assert(std::filesystem::is_empty(path));assert(rmdir(path.c_str())==0);}};
static std::vector<std::uint8_t> Packet(){int err=0;auto *e=opus_encoder_create(48000,1,OPUS_APPLICATION_VOIP,&err);assert(e&&err==OPUS_OK);std::int16_t pcm[960];for(unsigned i=0;i<960;++i)pcm[i]=std::int16_t(10000*std::sin(6.283185307*440*i/48000));std::vector<std::uint8_t> p(1275);int n=opus_encode(e,pcm,960,p.data(),p.size());assert(n>0);p.resize(n);opus_encoder_destroy(e);return p;}
static void Feed(g2h::Talkback &t,const std::vector<std::uint8_t>&p,unsigned n){for(unsigned i=0;i<n;++i)t.PushOpus(p.data(),p.size());}
int main(){av_log_set_level(AV_LOG_QUIET);FakeVendor vendor;const auto packet=Packet();
 // Mute races with completed disk write: the old generation may not commit.
 {Directory dir;g2h::Talkback t(dir.path);Gate gate;std::atomic<unsigned> hooks{0},processed{0};t.beforePublishCommit=[&]{if(hooks++==0)gate.Block();};t.afterEncode=[&]{++processed;};assert(t.Start());Feed(t,packet,35);gate.Await();const auto before=t.PublishedChunks();const auto completed=processed.load();t.SetMuted(true);t.SetMuted(false);gate.Release();Until([&]{return processed>completed;});std::this_thread::sleep_for(20ms);assert(t.PublishedChunks()==before);assert(vendor.calls==0);t.Stop();}
 // First chunk popped but not dispatched, plus two ready chunks: all are invalidated.
 {Directory dir;g2h::Talkback t(dir.path);Gate gate;std::atomic<unsigned> hooks{0};t.beforePlaybackDispatch=[&]{if(hooks++==0)gate.Block();};assert(t.Start());Feed(t,packet,35);gate.Await();Feed(t,packet,35);Until([&]{return t.PublishedChunks()>=2;});Feed(t,packet,35);Until([&]{return t.PublishedChunks()>=3;});const auto calls=vendor.calls.load();t.SetMuted(true);t.SetMuted(false);gate.Release();std::this_thread::sleep_for(30ms);assert(vendor.calls==calls);t.Stop();}
 // Partial AAC/PCM and a popped Opus packet straddle mute/unmute. Neither may
 // combine with the new epoch's audio to create an early chunk.
 {Directory dir;g2h::Talkback t(dir.path);Gate gate;std::atomic<unsigned> begun{0},processed{0};t.beforeEncode=[&]{if(++begun==13)gate.Block();};t.afterEncode=[&]{++processed;};assert(t.Start());Feed(t,packet,13);gate.Await();t.SetMuted(true);t.SetMuted(false);gate.Release();Until([&]{return processed==13;});Feed(t,packet,24);Until([&]{return processed==37;});assert(t.PublishedChunks()==0);const auto calls=vendor.calls.load();Feed(t,packet,16);Until([&]{return vendor.calls>calls;});assert(t.PublishedChunks()>0);t.Stop();}
 assert(vendor.invalid==0);assert(g2h::kFramesPerChunk==8);std::puts("Talkback mute generation, disk-commit, pending-playback and partial-codec tests passed");}
