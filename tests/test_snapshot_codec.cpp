#include "../app/src/g2h_snapshot_codec.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
static std::vector<uint8_t> read(const char *path){std::ifstream f(path,std::ios::binary);assert(f);return {std::istreambuf_iterator<char>(f),{}};}
static void dimensions(const std::vector<uint8_t>&j,unsigned w,unsigned h){
 assert(j.size()>=4&&j.size()<=64000&&j[0]==255&&j[1]==216&&j[j.size()-2]==255&&j.back()==217);bool found=false;
 for(size_t p=2;p+4<=j.size();){assert(j[p++]==255);auto code=j[p++];if(code==0xda||code==0xd9)break;size_t n=(j[p]<<8)|j[p+1];assert(n>=2&&p+n<=j.size());
  if(code==0xc0){assert(n>=8&&j[p+2]==8&&unsigned((j[p+3]<<8)|j[p+4])==h&&unsigned((j[p+5]<<8)|j[p+6])==w);found=true;}p+=n;}
 assert(found);
}
int main(int argc,char **argv){
 assert(argc==3||argc==4);av_log_set_level(AV_LOG_QUIET);auto frame=read(argv[1]);unsigned count=0;
 for(unsigned w:{320u,640u})for(unsigned q:{1u,70u,100u}){
  auto jpeg=g2h::SnapshotJpeg(frame,q,w,w*9/16);dimensions(jpeg,w,w*9/16);
  std::string path=std::string(argv[2])+"/snapshot-"+std::to_string(w)+"-"+std::to_string(q)+".jpg";
  std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(jpeg.data()),jpeg.size());assert(file);++count;
 }
 dimensions(g2h::SnapshotJpeg(frame,70),320,180); // Existing two-argument call.
 unsigned refused=0;auto reject=[&](unsigned w,unsigned h,unsigned q){try{(void)g2h::SnapshotJpeg(frame,q,w,h);}catch(...){++refused;return;}assert(false);};
 reject(0,0,70);reject(320,360,70);reject(640,180,70);reject(1280,720,70);reject(640,360,0);reject(640,360,101);assert(refused==6);
 if(argc==4){auto noise=read(argv[3]);bool capped=false;try{auto j=g2h::SnapshotJpeg(noise,100,640,360);dimensions(j,640,360);}catch(const std::runtime_error &e){capped=std::string(e.what())=="snapshot JPEG bounds";}assert(capped);}
 std::cout<<"SNAPSHOT_CODEC_OK "<<count<<" images; dimensions+quality+64k cap\n";
}
