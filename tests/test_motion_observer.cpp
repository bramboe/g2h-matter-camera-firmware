#define G2H_MOTION_TEST 1
#include "../app/src/g2h_motion_observer.hpp"
#include <filesystem>
#include <fstream>
#include <cassert>
#include <iostream>
namespace fs=std::filesystem;
using namespace g2h;
static void Text(const fs::path&p,const std::string&s){std::ofstream f(p);f<<s;assert(f.good());}
static void Word(std::array<unsigned char,1024>&b,unsigned at,std::uint32_t v){for(unsigned i=0;i<4;++i)b[at+i]=static_cast<unsigned char>((v>>(8*i))&255);}
struct Fixture{
    fs::path root,proc,exe,pid;std::string hash;
    Fixture(){char name[]="/tmp/g2h-motion-test-XXXXXX";auto p=mkdtemp(name);assert(p);root=fs::canonical(p);proc=root/"proc";exe=root/"camera";pid=proc/"211";fs::create_directories(pid);
        std::array<unsigned char,1024>b{};std::memcpy(b.data(),"\177ELF\1\1\1",7);b[16]=2;b[18]=40;Word(b,28,52);b[42]=32;b[44]=1;
        Word(b,52,1);Word(b,60,0x550000);Word(b,68,1024);Word(b,72,0x10000);Word(b,76,6);
        {std::ofstream f(exe,std::ios::binary);f.write(reinterpret_cast<char*>(b.data()),b.size());}chmod(exe.c_str(),0700);
        motion_detail::Fd fd(open(exe.c_str(),O_RDONLY));assert(motion_detail::Hash(fd.n,hash));
        fs::create_symlink(exe,pid/"exe");Text(pid/"comm","camera\n");Text(pid/"status","Name:\tcamera\nUid:\t0\t0\t0\t0\n");Birth(12345);
        Text(pid/"maps","00550000-00560000 rw-p 00000000 00:00 0\n");Set(0,1);
    }
    ~Fixture(){fs::remove_all(root);}
    void Birth(unsigned value){std::string s="211 (camera) S";for(unsigned i=0;i<18;++i)s+=" 0";Text(pid/"stat",s+" "+std::to_string(value)+"\n");}
    void Set(std::uint32_t value,unsigned char enabled){std::fstream f(pid/"mem",std::ios::in|std::ios::out|std::ios::binary);if(!f){Text(pid/"mem","");f.open(pid/"mem",std::ios::in|std::ios::out|std::ios::binary);}assert(f);unsigned char v[4]{};for(unsigned i=0;i<4;++i)v[i]=static_cast<unsigned char>(value>>(8*i));f.seekp(motion_detail::kValue);f.write(reinterpret_cast<char*>(v),4);f.seekp(motion_detail::kEnabled);f.write(reinterpret_cast<char*>(&enabled),1);assert(f.good());}
    MotionObserver Observer(){return MotionObserver(proc.string(),exe.string(),hash,getuid());}
};
int main(){
    Fixture f;auto observer=f.Observer();assert(observer.FindVendorPid()==211);
    auto first=observer.Observe(211);assert(first.Value()==false);assert(observer.HashAttempts()==1);
    f.Set(1,1);assert(observer.Observe(211).Value()==true);assert(observer.HashAttempts()==1);
    f.Set(0,1);assert(observer.Observe(211).Value()==false);assert(observer.HashAttempts()==1);
    assert(first.Value(first.sampledMs+kMotionFreshnessMs)==false);assert(!first.Value(first.sampledMs+kMotionFreshnessMs+1));assert(!first.Value(first.sampledMs-1));
    f.Set(1,0);assert(!observer.Observe(211).Value());
    f.Set(2,1);assert(!observer.Observe(211).Value());
    f.Set(1,1);Text(f.pid/"maps","00550000-00560000 r--p 00000000 00:00 0\n");assert(!observer.Observe(211).Value());
    Text(f.pid/"maps","00550000-00560000 rwxp 00000000 00:00 0\n");assert(observer.Observe(211).Value()==true);
    Text(f.pid/"maps","00550000-00560000 rwxs 00000000 00:00 0\n");assert(!observer.Observe(211).Value());
    Text(f.pid/"maps","00550000-00560000 rw-p 00000000 00:00 0\n");
    fs::rename(f.pid/"mem",f.pid/"mem.saved");assert(!observer.Observe(211).Value());fs::rename(f.pid/"mem.saved",f.pid/"mem");
    f.Birth(12346);assert(observer.Observe(211).Value()==true);assert(observer.HashAttempts()==2);
    Text(f.pid/"status","Uid:\t0\t1000\t0\t0\n");assert(!observer.Observe(211).Value());
    Text(f.pid/"status","Uid:\t0\t0\t0\t0\n");
    auto rejected=MotionObserver(f.proc.string(),f.exe.string(),std::string(64,'0'),getuid());
    assert(!rejected.Observe(211).Value());assert(!rejected.Observe(211).Value());assert(rejected.HashAttempts()==1);
    {std::fstream image(f.exe,std::ios::in|std::ios::out|std::ios::binary);image.seekp(500);image.put('X');}
    assert(!observer.Observe(211).Value());assert(observer.HashAttempts()==3);
    assert(!observer.Observe(211).Value());assert(observer.HashAttempts()==3);
    assert(!observer.Observe(1).Value());
    std::string unused;
    assert(motion_detail::HashDetailed(-1,unused)==motion_detail::HashStatus::ReadFailed);
    assert(motion_detail::HashDetailed(-1,unused,0)==motion_detail::HashStatus::Timeout);
    assert(motion_detail::StartTicks("211 (camera (x)) S"+std::string(" 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 99"),211)==99);
    assert(!motion_detail::StartTicks("212 (camera) S 0",211));
    std::cout<<"motion observer: held-state transitions, hash cache/restart, invalid/unavailable/stale and bounded identity guards passed\n";
}
