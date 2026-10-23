#pragma once
#include <string>
#include <vector>
namespace g2h {
inline constexpr unsigned kVideoWidth=1280,kVideoHeight=720,kVideoFps=20;
inline constexpr const char *kVideoPath="/11",*kVideoProfile="4d401f";
// RFC 6184 table 5: Main profile 4D with profile-iop 0x0x0000.
// 4d001f and the actual SPS 4d401f represent the same Main sub-profile.
enum class H264Subprofile {Unknown,Main,ConstrainedBaseline};
inline H264Subprofile H264Profile(const std::string &hex,unsigned &level){
    if(hex.size()!=6)return H264Subprofile::Unknown;unsigned number=0;
    for(char c:hex){unsigned d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:99;if(d>15)return H264Subprofile::Unknown;number=number*16+d;}
    unsigned idc=number>>16,iop=(number>>8)&255;level=number&255;
    if(idc==0x4d&&(iop&0xaf)==0)return H264Subprofile::Main;
    if((idc==0x42&&(iop&0x4f)==0x40)||(idc==0x4d&&(iop&0x8f)==0x80))return H264Subprofile::ConstrainedBaseline;
    return H264Subprofile::Unknown;
}
inline bool CompatibleH264(const std::vector<std::string>&fmtps,const std::string&actual){
    unsigned actualLevel=0;auto actualProfile=H264Profile(actual,actualLevel);
    if(actualProfile==H264Subprofile::Unknown||actualLevel!=31)return false;
    bool profile=false,fragmentation=false;
    for(const auto &line:fmtps){
        size_t pos=0;
        while(pos<line.size()){
            auto end=line.find(';',pos);auto value=line.substr(pos,end==std::string::npos?std::string::npos:end-pos);
            auto begin=value.find_first_not_of(" \t"),last=value.find_last_not_of(" \t");
            if(begin!=std::string::npos)value=value.substr(begin,last-begin+1);
            if(value.rfind("profile-level-id=",0)==0){
                if(profile)return false;unsigned level=0;auto offered=H264Profile(value.substr(17),level);
                if(offered!=actualProfile||level<actualLevel||level>52)return false;profile=true;
            }
            if(value.rfind("packetization-mode=",0)==0){if(fragmentation||value!="packetization-mode=1")return false;fragmentation=true;}
            if(end==std::string::npos)break;pos=end+1;
        }
    }return profile&&fragmentation;
}
inline bool CompatibleMainH264(const std::vector<std::string>&fmtps){return CompatibleH264(fmtps,kVideoProfile);}
}
