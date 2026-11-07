#include "../app/src/g2h_video_control.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
namespace fs=std::filesystem;
static void write(const fs::path &path,const std::string &value){fs::create_directories(path.parent_path());std::ofstream(path)<<value;assert(!chmod(path.c_str(),0600));}
int main(int argc,char **argv){
 assert(argc==2);const fs::path root=argv[1],cfg=root/"config",proc=root/"proc",pause=root/"pause";fs::create_directories(cfg);fs::create_directories(pause);
 write(cfg/"video-control-installed","g2h-video-boot-v1\n");write(pause/"paused","");
 auto hardware=[&](unsigned enabled,unsigned frames){
  write(proc/"mstar/ISP/DevInfo/FrameNumber",std::to_string(frames)+"\n");
  for(unsigned i=0;i<2;++i){const auto id=std::to_string(i);
   for(auto suffix:{"VENC/VeChn"+id+"/Create","VENC/VeChn"+id+"/Start","VI/ViChn"+id+"/Enable"})write(proc/("mstar/MI/"+suffix),std::to_string(i==0?enabled:0)+"\n");}
 };
 const fs::path helper=root/"helper";write(helper,"#!/bin/sh\nexit 1\n");assert(!chmod(helper.c_str(),0700));
 auto control=[&]{return g2h::VideoControl(cfg,helper,proc,pause);};
 hardware(0,0);write(cfg/"video-enabled","0\n");
 {auto c=control();assert(c.Available()&&c.Observe()==std::optional<bool>(false));}
 hardware(1,10);
 {auto c=control();assert(!c.Observe());} // A requested OFF is not an observed OFF.
 write(cfg/"video-enabled","1\n");
 {auto c=control();assert(!c.Observe());std::this_thread::sleep_for(std::chrono::milliseconds(1050));hardware(1,11);assert(c.Observe()==std::optional<bool>(true));
  assert(c.Request(false));assert(!c.Observe()); // Pending cannot echo the request.
 }
 hardware(0,0);write(cfg/"video-enabled","0\n");fs::remove(pause/"paused");
 {auto c=control();assert(!c.Observe());} // Zero counters alone are insufficient.
 write(pause/"paused","");fs::remove(cfg/"video-enabled");fs::create_symlink(root/"outside",cfg/"video-enabled");
 {auto c=control();assert(!c.Observe());}
 fs::remove(cfg/"video-enabled");write(cfg/"video-enabled","invalid\n");
 {auto c=control();assert(!c.Observe());}
 assert(!chmod((cfg/"video-control-installed").c_str(),0644));
 {auto c=control();assert(!c.Available()&&!c.Request(false));}
 std::cout<<"video control observation tests passed\n";
}
