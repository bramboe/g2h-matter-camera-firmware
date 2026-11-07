// Synthetic Unix-server tests only; no camera/video decoder dependency.
#include "g2h_media_input.hpp"
#include <cstdio>
#include <cstdlib>
int main(int argc,char **argv){
 if(argc!=5)return 2;const int attempts=std::atoi(argv[3]),records=std::atoi(argv[4]);
 if(attempts<1||attempts>2||records<0||records>3)return 2;
 setenv("G2H_MEDIA_BROKER_DIR",argv[1],1);g2h::MediaInput input(std::atoi(argv[2])!=0);int failures=0;
 for(int i=0;i<attempts;++i){
  try{input.Start();std::printf("READY %u %u %zu %zu\n",input.AudioTrack().payload,input.AudioTrack().channel,input.parameters.sps.size(),input.parameters.pps.size());
   for(int j=0;j<records;++j){input.RenewReadDeadline();std::uint8_t header[4];input.Read(header,4);const std::size_t n=(unsigned(header[2])<<8)|header[3];if(header[0]!='$'||!n||n+4>g2h::media::kMaxRecord)throw std::runtime_error("bad record");std::vector<std::uint8_t> payload(n);input.Read(payload.data(),n);std::printf("RECORD %zu %u %u\n",n,payload.front(),payload.back());}
  }catch(...){++failures;std::puts("ERROR");}
  input.Stop();if(!input.parameters.sps.empty()||input.AudioTrack().payload)return 3;
 }
 return failures;
}
