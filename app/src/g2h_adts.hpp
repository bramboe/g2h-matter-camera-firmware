#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace g2h {
// The vendor playfile reader copies each ADTS frame into a 1024-byte stack buffer
// without a size check; frames we hand it stay well below that.
inline constexpr std::size_t kMaxAdtsFrame=1000;

// True when data is only AAC-LC 16 kHz mono ADTS frames of at most kMaxAdtsFrame bytes.
inline bool ValidAdts(const std::vector<std::uint8_t> &data){
    std::size_t at=0;
    while(at<data.size()){
        if(data.size()-at<7)return false;
        const auto *h=data.data()+at;
        if(h[0]!=0xff||(h[1]&0xf6)!=0xf0||((h[2]>>6)&3)!=1||((h[2]>>2)&15)!=8||(((h[2]&1)<<2)|(h[3]>>6))!=1)return false;
        const std::size_t len=(std::size_t(h[3]&3)<<11)|(std::size_t(h[4])<<3)|(h[5]>>5);
        if(len<8||len>kMaxAdtsFrame||len>data.size()-at)return false;
        at+=len;
    }
    return at>0;
}
}
