#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace g2h {
// RFC 3640 AAC-hbr depacketizer for the G2H /13 audio track (sizelength=13,
// indexlength=indexdeltalength=3). Interleaving is refused; one fragmented AU
// is reassembled within a fixed bound. Emits complete raw AAC access units.
class AacHbrDepacketizer {
public:
    static constexpr std::size_t kMaxAu=2048,kMaxAusPerPacket=8;
    enum class Result {Accepted,Rejected};
    explicit AacHbrDepacketizer(unsigned payloadType):payloadType_(payloadType){fragment_.reserve(kMaxAu);}
    template<class Emit> Result Push(const std::uint8_t *packet,std::size_t size,Emit &&emit){
        if(!packet||size<12||(packet[0]>>6)!=2)return Reject();
        const bool padding=packet[0]&0x20,extension=packet[0]&0x10;const unsigned csrc=packet[0]&0x0f;
        const bool marker=packet[1]&0x80;
        if((packet[1]&0x7f)!=payloadType_)return Reject();
        const std::uint32_t timestamp=(std::uint32_t(packet[4])<<24)|(std::uint32_t(packet[5])<<16)|(std::uint32_t(packet[6])<<8)|packet[7];
        std::size_t at=12+4*std::size_t(csrc),end=size;
        if(at>end)return Reject();
        if(extension){
            if(at+4>end)return Reject();
            at+=4+4*((std::size_t(packet[at+2])<<8)|packet[at+3]);
            if(at>end)return Reject();
        }
        if(padding){const std::size_t pad=packet[end-1];if(!pad||pad>end-at)return Reject();end-=pad;}
        if(end-at<2)return Reject();
        const std::size_t headerBits=(std::size_t(packet[at])<<8)|packet[at+1];at+=2;
        if(!headerBits||headerBits%16)return Reject();
        const std::size_t count=headerBits/16;
        if(count>kMaxAusPerPacket||at+2*count>end)return Reject();
        std::size_t sizes[kMaxAusPerPacket];
        for(std::size_t i=0;i<count;++i){
            const unsigned header=(unsigned(packet[at+2*i])<<8)|packet[at+2*i+1];
            if(header&7)return Reject(); // AU-index / AU-index-delta 0: no interleaving
            sizes[i]=header>>3;
            if(!sizes[i]||sizes[i]>kMaxAu)return Reject();
        }
        at+=2*count;const std::size_t data=end-at;
        // RFC 3640 §3.2.3: each fragment repeats the one AU header; marker ends it.
        if(count==1&&sizes[0]>data){
            if(fragment_.empty()){fragmentSize_=sizes[0];fragmentTimestamp_=timestamp;}
            else if(sizes[0]!=fragmentSize_||timestamp!=fragmentTimestamp_)return Reject();
            if(fragment_.size()+data>fragmentSize_)return Reject();
            fragment_.insert(fragment_.end(),packet+at,packet+end);
            if(!marker)return Result::Accepted;
            if(fragment_.size()!=fragmentSize_)return Reject();
            emit(fragment_.data(),fragment_.size());fragment_.clear();return Result::Accepted;
        }
        fragment_.clear(); // a lost last fragment drops that AU only
        std::size_t total=0;for(std::size_t i=0;i<count;++i)total+=sizes[i];
        if(total!=data)return Reject();
        for(std::size_t i=0;i<count;++i){emit(packet+at,sizes[i]);at+=sizes[i];}
        return Result::Accepted;
    }
private:
    Result Reject(){fragment_.clear();return Result::Rejected;}
    const unsigned payloadType_;
    std::vector<std::uint8_t> fragment_;
    std::size_t fragmentSize_=0;
    std::uint32_t fragmentTimestamp_=0;
};
}
