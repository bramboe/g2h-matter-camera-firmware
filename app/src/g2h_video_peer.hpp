#include "g2h_video_profile.hpp"
#pragma once
#include <rtc/rtc.hpp>
#include <functional>
#include <memory>
#include <atomic>
#include <stdexcept>
#include <cstdint>
#include <vector>
#include <cstring>
#include <cctype>
#include <string>

namespace g2h {
// One video peer with an optional send-only Opus track. Signaling stays in
// memory and is delivered to the owner; the owner must marshal it back onto the
// Matter event thread.
class VideoPeer {
public:
    using DescriptionCallback=std::function<void(std::string, bool)>;
    using StateCallback=std::function<void(rtc::PeerConnection::State)>;
    using TalkCallback=std::function<void(const std::uint8_t*,std::size_t)>;
    // talk: a sendrecv audio offer also gets a receive path; incoming Opus frames go
    // to the callback (on a library thread, it must not block).
    VideoPeer(const rtc::Configuration &config,DescriptionCallback description,StateCallback state={},std::string profile=kVideoProfile,bool audio=false,TalkCallback talk={})
        : peer_(std::make_shared<rtc::PeerConnection>(ManualConfiguration(config))),profile_(std::move(profile)),audio_(audio),talk_(std::move(talk)) {
        unsigned level=0;if(H264Profile(profile_,level)==H264Subprofile::Unknown||level!=31)throw std::invalid_argument("unapproved encoder profile");
        if(!description||config.disableFingerprintVerification)throw std::invalid_argument("secure description callback required");
        std::weak_ptr<rtc::PeerConnection> weak=peer_;
        peer_->onGatheringStateChange([weak,description](rtc::PeerConnection::GatheringState state){
            if(state!=rtc::PeerConnection::GatheringState::Complete)return;
            if(auto peer=weak.lock()){
                auto d=peer->localDescription();
                try {if(d)description(std::string(*d),d->type()==rtc::Description::Type::Offer);}
                catch(...) {peer->close();}
            }
        });
        if(state)peer_->onStateChange([state](rtc::PeerConnection::State value){try{state(value);}catch(...){}});
    }
    ~VideoPeer(){Close();}
    VideoPeer(const VideoPeer&)=delete;
    VideoPeer& operator=(const VideoPeer&)=delete;
    void Offer(){
        if(track_||closed_)throw std::logic_error("negotiation already started");
        AddTrack("video",96);if(audio_)AddAudioTrack("audio",111,bool(talk_));
        peer_->setLocalDescription(rtc::Description::Type::Offer);
    }
    void Answer(const std::string &sdp){
        CheckDescription(sdp);
        if(track_||closed_)throw std::logic_error("negotiation already started");
        rtc::Description offer(sdp,rtc::Description::Type::Offer);
        if(offer.mediaCount()>2)throw std::invalid_argument("too many media sections");
        std::string mid;int payload=-1;
        for(unsigned i=0;i<offer.mediaCount();++i){
            auto entry=offer.media(i);
            auto video=std::get_if<rtc::Description::Media*>(&entry);
            if(!video)throw std::invalid_argument("data channels are not supported");
            if((*video)->isRemoved())continue;
            if((*video)->type()=="audio"){
                if(audioTrack_||!rejectedTracks_.empty())throw std::invalid_argument("too many audio sections");
                const auto direction=(*video)->direction();
                const bool receives=direction==rtc::Description::Direction::RecvOnly||direction==rtc::Description::Direction::SendRecv;
                const int opus=receives&&audio_?OpusPayload(**video):-1;
                if(opus>=0&&!(*video)->mid().empty()&&(*video)->mid().size()<=32){
                    AddAudioTrack((*video)->mid(),opus,talk_&&direction==rtc::Description::Direction::SendRecv);continue;}
                // Pre-register a rejected audio description, otherwise the library
                // automatically creates an incoming track before composing its answer.
                auto rejected=(*video)->reciprocate();rejected.markRemoved();
                rejectedTracks_.push_back(peer_->addTrack(rejected));continue;
            }
            if((*video)->type()!="video"||payload!=-1)throw std::invalid_argument("video-only offer required");
            if((*video)->direction()!=rtc::Description::Direction::RecvOnly&&(*video)->direction()!=rtc::Description::Direction::SendRecv)
                throw std::invalid_argument("remote must receive video");
            mid=(*video)->mid();
            for(int pt:(*video)->payloadTypes()){
                const auto *map=(*video)->rtpMap(pt);
                if(map&&map->format=="H264"&&map->clockRate==90000&&CompatibleParameters(map->fmtps)){payload=pt;break;}
            }
            if(payload<0)throw std::invalid_argument("compatible Main H264 offer required");
        }
        if(payload<0||mid.empty()||mid.size()>32)throw std::invalid_argument("video track absent");
        AddTrack(mid,payload);
        peer_->setRemoteDescription(offer);
        peer_->setLocalDescription(rtc::Description::Type::Answer);
    }
    void AcceptAnswer(const std::string &sdp){
        CheckDescription(sdp);
        if(!track_||closed_)throw std::logic_error("offer not active");
        rtc::Description answer(sdp,rtc::Description::Type::Answer);
        bool matched=false;
        for(unsigned i=0;i<answer.mediaCount();++i){
            auto entry=answer.media(i);
            auto video=std::get_if<rtc::Description::Media*>(&entry);
            if(!video)throw std::invalid_argument("data channels are not supported");
            if((*video)->isRemoved())continue;
            if(audioTrack_&&(*video)->type()=="audio"&&(*video)->mid()==audioTrack_->description().mid()){
                auto *opus=(*video)->rtpMap(audioPayload_);
                if(!opus||!IsOpus(*opus))throw std::invalid_argument("unsupported answer audio codec");
                continue;
            }
            if(matched||(*video)->type()!="video"||(*video)->mid()!=track_->description().mid())throw std::invalid_argument("unexpected answer track");
            auto *map=(*video)->rtpMap(payload_);
            if(!map||map->format!="H264"||map->clockRate!=90000||!CompatibleParameters(map->fmtps))throw std::invalid_argument("unsupported answer codec");
            if((*video)->direction()!=rtc::Description::Direction::RecvOnly&&(*video)->direction()!=rtc::Description::Direction::SendRecv)
                throw std::invalid_argument("remote must receive video");
            matched=true;
        }
        if(!matched)throw std::invalid_argument("video answer absent");
        peer_->setRemoteDescription(answer);
    }
    void AddCandidate(const std::string &candidate,const std::string &mid){
        if(closed_||candidate.size()>2048||candidate.find_first_of("\r\n\0",0,3)!=std::string::npos||mid.empty()||mid.size()>32)
            throw std::invalid_argument("invalid ICE candidate");
        peer_->addRemoteCandidate(rtc::Candidate(candidate,mid));
    }
    bool Ready() const {return !closed_&&track_&&track_->isOpen()&&peer_->state()==rtc::PeerConnection::State::Connected;}
    void Send(const std::vector<std::uint8_t> &annexB,std::uint32_t rtpTimestamp){
        if(!Ready()||annexB.empty()||annexB.size()>512*1024)throw std::runtime_error("video not ready or oversized");
        rtc::binary bytes(annexB.size());std::memcpy(bytes.data(),annexB.data(),annexB.size());
        track_->sendFrame(std::move(bytes),rtc::FrameInfo(rtpTimestamp));
    }
    bool AudioReady() const {return Ready()&&audioTrack_&&audioTrack_->isOpen();}
    void SendAudio(const std::vector<std::uint8_t> &opus,std::uint32_t rtpTimestamp){
        if(!AudioReady()||opus.empty()||opus.size()>1275)throw std::runtime_error("audio not ready or oversized");
        rtc::binary bytes(opus.size());std::memcpy(bytes.data(),opus.data(),opus.size());
        audioTrack_->sendFrame(std::move(bytes),rtc::FrameInfo(rtpTimestamp));
    }
    void Close() noexcept {if(!closed_.exchange(true)){try{peer_->close();}catch(...){}}}
    const std::shared_ptr<rtc::PeerConnection>& Peer() const {return peer_;}
private:
    static rtc::Configuration ManualConfiguration(rtc::Configuration config){
        // Offer()/Answer() explicitly drive SDP. With the library default it
        // answers automatically, so our explicit answer would be a second one.
        config.disableAutoNegotiation=true;return config;
    }
    static void CheckDescription(const std::string &sdp){
        if(sdp.empty()||sdp.size()>16384||sdp.find('\0')!=std::string::npos)throw std::invalid_argument("invalid SDP size");
    }
    bool CompatibleParameters(const std::vector<std::string>&fmtps)const{return g2h::CompatibleH264(fmtps,profile_);}
    void AddTrack(const std::string &mid,int payload){
        payload_=payload;
        rtc::Description::Video media(mid,rtc::Description::Direction::SendOnly);
        media.addH264Codec(payload,"profile-level-id="+profile_+";packetization-mode=1;level-asymmetry-allowed=1");
        media.addSSRC(427,"g2h-camera","g2h-camera","video");
        track_=peer_->addTrack(media);
        auto rtp=std::make_shared<rtc::RtpPacketizationConfig>(427,"g2h-camera",payload,rtc::H264RtpPacketizer::ClockRate);
        rtp->mid=mid;
        auto packetizer=std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence,rtp,1200);
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp));
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        track_->setMediaHandler(packetizer);
    }
    static bool IsOpus(const rtc::Description::Media::RtpMap &map){
        std::string format=map.format;for(auto &c:format)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return format=="opus"&&map.clockRate==48000;
    }
    static int OpusPayload(const rtc::Description::Media &media){
        for(int pt:media.payloadTypes()){const auto *map=media.rtpMap(pt);if(map&&IsOpus(*map))return pt;}
        return -1;
    }
    void AddAudioTrack(const std::string &mid,int payload,bool receive=false){
        audioPayload_=payload;
        rtc::Description::Audio media(mid,receive?rtc::Description::Direction::SendRecv:rtc::Description::Direction::SendOnly);
        media.addOpusCodec(payload,std::string("minptime=10;useinbandfec=1"));
        media.addSSRC(428,"g2h-camera","g2h-camera","audio");
        audioTrack_=peer_->addTrack(media);
        auto rtp=std::make_shared<rtc::RtpPacketizationConfig>(428,"g2h-camera",payload,rtc::OpusRtpPacketizer::DefaultClockRate);
        rtp->mid=mid;
        auto packetizer=std::make_shared<rtc::OpusRtpPacketizer>(rtp);
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp));
        if(receive){
            // Incoming messages run from the END of the chain backwards: the receiving
            // session must see raw RTP first, so it goes last and the depacketizer before it.
            // (The reverse order dropped every frame as "malformed RTP".)
            packetizer->addToChain(std::make_shared<rtc::OpusRtpDepacketizer>());
            packetizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
            audioTrack_->onFrame([talk=talk_](rtc::binary frame,rtc::FrameInfo){
                if(frame.empty()||frame.size()>1275)return;
                try{talk(reinterpret_cast<const std::uint8_t*>(frame.data()),frame.size());}catch(...){}
            });
        }
        audioTrack_->setMediaHandler(packetizer);
    }
    std::shared_ptr<rtc::PeerConnection> peer_;
    std::shared_ptr<rtc::Track> track_,audioTrack_;
    std::vector<std::shared_ptr<rtc::Track>> rejectedTracks_;
    std::atomic<bool> closed_{false};
    int payload_=-1,audioPayload_=-1;
    std::string profile_;
    bool audio_=false;
    TalkCallback talk_;
};
}
