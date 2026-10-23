#pragma once
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace g2h {
// Bounded SPS reader for the 8-bit 4:2:0 progressive Baseline/Main encoders.
// No metadata is rewritten: dimensions and the actual profile come from SPS.
inline bool IsSupported720pSps(const std::vector<std::uint8_t>&sps){
    try{
        if(sps.size()<5||sps.size()>2048||sps[0]!=0x67||sps[3]!=31)return false;
        const auto idc=sps[1],iop=sps[2];
        if(!((idc==66&&(iop&0x40)&&(iop&0x0f)==0)||(idc==77&&((iop&0xaf)==0||(iop&0x8f)==0x80))))return false;
        std::vector<std::uint8_t> rbsp;unsigned zeros=0;
        for(std::size_t i=1;i<sps.size();++i){auto b=sps[i];if(zeros>=2&&b==3){if(i+1==sps.size()||sps[i+1]>3)return false;zeros=0;continue;}rbsp.push_back(b);zeros=b==0?zeros+1:0;}
        struct Bits{
            const std::vector<std::uint8_t>&data;std::size_t pos=0;
            unsigned bit(){if(pos>=data.size()*8)throw std::out_of_range("SPS bits");auto value=(data[pos/8]>>(7-pos%8))&1;++pos;return value;}
            unsigned ue(){unsigned n=0;while(!bit()){if(++n>24)throw std::out_of_range("SPS expgolomb");}unsigned v=1;while(n--)v=(v<<1)|bit();return v-1;}
            int se(){unsigned v=ue();return v&1?int((v+1)/2):-int(v/2);}
        }bits{rbsp};
        bits.pos=24;
        if(bits.ue()>31||bits.ue()>12)return false;
        auto order=bits.ue();if(order>2)return false;
        if(order==0){if(bits.ue()>12)return false;}
        if(order==1){bits.bit();auto a=bits.se(),b=bits.se();if(a< -32768||a>32767||b< -32768||b>32767)return false;auto count=bits.ue();if(count>255)return false;while(count--){auto offset=bits.se();if(offset< -32768||offset>32767)return false;}}
        if(bits.ue()>16)return false;bits.bit();
        auto wm=bits.ue(),hm=bits.ue();if(wm>255||hm>255||bits.bit()!=1)return false;
        bits.bit();unsigned left=0,right=0,top=0,bottom=0;
        if(bits.bit()){left=bits.ue();right=bits.ue();top=bits.ue();bottom=bits.ue();}
        const unsigned width=(wm+1)*16,height=(hm+1)*16;
        if(left>width/2||right>width/2||top>height/2||bottom>height/2||2*(left+right)>=width||2*(top+bottom)>=height)return false;
        return width-2*(left+right)==1280&&height-2*(top+bottom)==720;
    }catch(...){return false;}
}
}
