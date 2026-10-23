#pragma once
#include "h264_rtp.hpp"
#include <string>
#include <utility>
namespace g2h {
class H264Parameters {
    static bool Decode(const std::string &s,std::vector<std::uint8_t>&out){
        if(s.empty()||s.size()%4||s.size()>2732)return false;
        const std::string alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        out.clear();
        for(std::size_t at=0;at<s.size();at+=4){
            unsigned v[4]={};unsigned padding=0;
            for(unsigned i=0;i<4;++i){
                if(s[at+i]=='='){if(i<2||at+4!=s.size())return false;++padding;}
                else {if(padding)return false;auto n=alphabet.find(s[at+i]);if(n==std::string::npos)return false;v[i]=static_cast<unsigned>(n);}
            }
            if((padding==2&&(v[1]&15))||(padding==1&&(v[2]&3)))return false;
            out.push_back((v[0]<<2)|(v[1]>>4));
            if(padding<2)out.push_back((v[1]<<4)|(v[2]>>2));
            if(!padding)out.push_back((v[2]<<6)|v[3]);
        }
        return out.size()<=2048;
    }
public:
    std::vector<std::uint8_t> sps,pps;
    bool Parse(const std::string &sdp){
        sps.clear();pps.clear();
        if(sdp.size()>8192||sdp.find("a=rtpmap:36 H264/90000")==std::string::npos)return false;
        auto line=sdp.find("a=fmtp:36 ");if(line==std::string::npos)return false;
        auto end=sdp.find_first_of("\r\n",line);if(end==std::string::npos)end=sdp.size();
        const std::string label="sprop-parameter-sets=";
        auto at=sdp.find(label,line);if(at==std::string::npos||at>=end)return false;at+=label.size();
        auto last=sdp.find_first_of("; \r\n",at);if(last==std::string::npos||last>end)last=end;
        auto comma=sdp.find(',',at);if(comma==std::string::npos||comma>=last)return false;
        std::vector<std::uint8_t> a,b;
        if(!Decode(sdp.substr(at,comma-at),a)||!Decode(sdp.substr(comma+1,last-comma-1),b))return false;
        if(a.size()<4||b.empty()||(a[0]&159)!=7||(b[0]&159)!=8)return false;
        sps=std::move(a);pps=std::move(b);return true;
    }
    std::string ProfileLevel()const{
        if(sps.size()<4)return {};
        const char*hex="0123456789abcdef";std::string out;
        for(unsigned i=1;i<4;++i){out+=hex[sps[i]>>4];out+=hex[sps[i]&15];}return out;
    }
    bool PrependToIdr(H264Rtp::Frame &frame)const{
        if(!frame.idr)return true;
        if(sps.empty()||pps.empty()||frame.annexB.size()+sps.size()+pps.size()+8>H264Rtp::kMaxFrame)return false;
        std::vector<std::uint8_t> out;out.reserve(frame.annexB.size()+sps.size()+pps.size()+8);
        for(const auto*p:{&sps,&pps}){out.insert(out.end(),{0,0,0,1});out.insert(out.end(),p->begin(),p->end());}
        out.insert(out.end(),frame.annexB.begin(),frame.annexB.end());frame.annexB=std::move(out);frame.nals+=2;return true;
    }
};
}
