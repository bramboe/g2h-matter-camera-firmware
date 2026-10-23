#pragma once
#include "g2h_video_delegate.hpp"
#include "g2h_video_peer.hpp"
#include <app/clusters/webrtc-transport-provider-server/WebRTCTransportProviderCluster.h>
#include <lib/core/NodeId.h>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace g2h {
namespace wp=chip::app::Clusters::WebRTCTransportProvider;
// Every public operation runs on the owner's Matter event thread. RTC workers
// only write a bounded mailbox; they never call SDK cluster methods.
class WebRtcDelegate final : public wp::Delegate {
public:
    struct Target {std::uint16_t session;chip::ScopedNodeId peer;chip::EndpointId endpoint;std::uint64_t epoch;};
    struct Hooks {
        // Must arrange periodic Poll() calls on the same event thread.
        std::function<CHIP_ERROR()> ensurePolling;
        // Send Offer/Answer over authenticated Matter CASE. Failure callbacks
        // may run later; their epoch token cannot end a replacement session.
        std::function<CHIP_ERROR(const Target&,const std::string&,bool,std::function<void()>)> signal;
        std::function<void(const Target&)> cancelSignal;
        std::function<CHIP_ERROR(bool&)> utcIsNull;
        std::function<std::uint64_t()> nowMs;
        std::function<std::string()> encoderProfile;
        // Two-way talk: whether a session may receive controller audio, and where it goes.
        std::function<bool()> talkAllowed;
        VideoPeer::TalkCallback talk;
    };
    WebRtcDelegate(VideoDelegate &video,rtc::Configuration config,Hooks hooks)
        : video_(video),config_(std::move(config)),hooks_(std::move(hooks)),owner_(std::this_thread::get_id()),mail_(std::make_shared<Mailbox>()){}
    ~WebRtcDelegate() override {Close(false);}
    WebRtcDelegate(const WebRtcDelegate&)=delete;
    WebRtcDelegate& operator=(const WebRtcDelegate&)=delete;
    void SetCluster(wp::WebRTCTransportProviderCluster *cluster){cluster_=cluster;}
    void Shutdown(){Close(true);}
    void Poll(){
        if(!Owner()||!session_)return;
        std::deque<Event> events;bool failed=false;
        {std::lock_guard<std::mutex> lock(mail_->mutex);events.swap(mail_->events);failed=mail_->failed;mail_->failed=false;}
        if(failed||!video_.Enabled()){lastStage_=failed?"mailbox-overflow":"video-disabled";Close(true);return;}
        for(auto &event:events){
            if(!session_||event.epoch!=target_.epoch)continue;
            if(event.kind==Kind::Description){
                auto weak=std::weak_ptr<Mailbox>(mail_);auto epoch=target_.epoch;
                auto fail=[weak,epoch]{Push(weak,Event{epoch,Kind::SignalFailed,{},{}});};
                CHIP_ERROR err=CHIP_ERROR_INTERNAL;
                try {if(hooks_.signal)err=hooks_.signal(target_,event.sdp,event.offer,std::move(fail));}catch(...){}
                if(err!=CHIP_NO_ERROR){lastStage_="signal-rejected";Close(true);return;}
                lastStage_="signal-pending";
            }else if(event.kind==Kind::Connected){connected_=true;lastFrame_=Now();lastStage_="connected";}
            else {lastStage_=event.kind==Kind::SignalFailed?"signal-failed":"peer-failed";Close(true);return;}
        }
        auto now=Now();
        // Keep whether our description was still being delivered over CASE.
        if(session_&&!connected_&&now-started_>=30000){
            lastStage_=std::strcmp(lastStage_,"signal-pending")?"negotiation-timeout":"negotiation-timeout-signal-pending";Close(true);}
        else if(session_&&connected_&&now-lastFrame_>=5000){lastStage_="media-idle-timeout";Close(true);}
    }
    CHIP_ERROR SendFrame(const std::vector<std::uint8_t>&frame,std::uint32_t stamp){
        if(!Owner()||!session_||!video_.Active()||!session_->Ready())return CHIP_ERROR_INCORRECT_STATE;
        try {session_->Send(frame,stamp);lastFrame_=Now();return CHIP_NO_ERROR;}catch(...){Close(true);return CHIP_ERROR_INTERNAL;}
    }
    bool Ready() const {return session_&&video_.Active()&&session_->Ready();}
    bool AudioReady() const {return Ready()&&audio_&&session_->AudioReady();}
    CHIP_ERROR SendAudio(const std::vector<std::uint8_t>&opus,std::uint32_t stamp){
        if(!Owner()||!AudioReady())return CHIP_ERROR_INCORRECT_STATE;
        try {session_->SendAudio(opus,stamp);return CHIP_NO_ERROR;}catch(...){return CHIP_ERROR_INTERNAL;} // audio loss never ends video
    }
    bool HasSession() const {return session_!=nullptr;}
    const char *LastStage() const {return lastStage_;}
    bool IsOwnerThread() const {return Owner();}
    std::string PeerAddress() const {return session_?session_->Peer()->remoteAddress().value_or(""):std::string();}
    CHIP_ERROR HandleSolicitOffer(const OfferRequestArgs &args,wp::WebRTCSessionStruct &out,bool &deferred) override {
        auto err=Begin(args,nullptr,out);if(err==CHIP_NO_ERROR)deferred=false;return err;
    }
    CHIP_ERROR HandleProvideOffer(const ProvideOfferRequestArgs &args,wp::WebRTCSessionStruct &out) override {return Begin(args,&args.sdp,out);}
    CHIP_ERROR HandleProvideAnswer(std::uint16_t id,const std::string &sdp) override {
        if(!Owner()||!Match(id)||!offering_||answered_)return CHIP_ERROR_INCORRECT_STATE;
        try {session_->AcceptAnswer(sdp);answered_=true;return CHIP_NO_ERROR;}catch(...){Close(true);return CHIP_ERROR_INVALID_ARGUMENT;}
    }
    CHIP_ERROR HandleProvideICECandidates(std::uint16_t id,const std::vector<wp::ICECandidateStruct>&candidates) override {
        if(!Owner()||!Match(id))return CHIP_ERROR_INCORRECT_STATE;
        if(candidates.size()>16)return CHIP_ERROR_INVALID_ARGUMENT;
        try {
            for(const auto &candidate:candidates){
                if(candidate.SDPMid.IsNull()||candidate.candidate.size()>2048||candidate.SDPMid.Value().size()>32)return CHIP_ERROR_INVALID_ARGUMENT;
            }
            for(const auto &candidate:candidates)session_->AddCandidate(
                std::string(candidate.candidate.data(),candidate.candidate.size()),
                std::string(candidate.SDPMid.Value().data(),candidate.SDPMid.Value().size()));
            return CHIP_NO_ERROR;
        }catch(...){Close(true);return CHIP_ERROR_INVALID_ARGUMENT;}
    }
    CHIP_ERROR HandleEndSession(std::uint16_t id,wp::WebRTCEndReasonEnum) override {
        if(!Owner()||!Match(id))return CHIP_ERROR_NOT_FOUND;
        lastStage_="ended-by-peer";Close(false);return CHIP_NO_ERROR; // SDK removes its list entry after this returns.
    }
    CHIP_ERROR ValidateStreamUsage(wp::StreamUsageEnum usage,chip::Optional<std::vector<std::uint16_t>>&videos,
                                   chip::Optional<std::vector<std::uint16_t>>&audios) override {
        if(IsStreamUsageSupported(usage)!=CHIP_NO_ERROR)return CHIP_ERROR_INVALID_ARGUMENT;
        if(!videos.HasValue())videos.SetValue(std::vector<std::uint16_t>{1});
        if(!audios.HasValue())audios.SetValue(video_.AudioAllocated(VideoDelegate::kAudioStreamId)?
            std::vector<std::uint16_t>{VideoDelegate::kAudioStreamId}:std::vector<std::uint16_t>{});
        if(ValidateVideoStreams(videos.Value())!=CHIP_NO_ERROR||ValidateAudioStreams(audios.Value())!=CHIP_NO_ERROR)return CHIP_ERROR_INVALID_ARGUMENT;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR ValidateVideoStreamID(std::uint16_t id) override {return video_.Allocated(id)&&video_.Enabled()?CHIP_NO_ERROR:CHIP_ERROR_NOT_FOUND;}
    CHIP_ERROR ValidateAudioStreamID(std::uint16_t id) override {return video_.AudioAllocated(id)&&video_.Enabled()?CHIP_NO_ERROR:CHIP_ERROR_NOT_FOUND;}
    CHIP_ERROR ValidateVideoStreams(const std::vector<std::uint16_t>&ids) override {return ids.size()==1?ValidateVideoStreamID(ids[0]):CHIP_ERROR_INVALID_ARGUMENT;}
    CHIP_ERROR ValidateAudioStreams(const std::vector<std::uint16_t>&ids) override {
        if(ids.empty())return CHIP_NO_ERROR;
        return ids.size()==1?ValidateAudioStreamID(ids[0]):CHIP_ERROR_INVALID_ARGUMENT;
    }
    CHIP_ERROR IsStreamUsageSupported(wp::StreamUsageEnum usage) override {return usage==wp::StreamUsageEnum::kLiveView?CHIP_NO_ERROR:CHIP_ERROR_NOT_FOUND;}
    CHIP_ERROR IsHardPrivacyModeActive(bool &active) override {active=false;return CHIP_NO_ERROR;}
    CHIP_ERROR IsSoftRecordingPrivacyModeActive(bool &active) override {active=false;return CHIP_NO_ERROR;}
    CHIP_ERROR IsSoftLivestreamPrivacyModeActive(bool &active) override {active=video_.LivestreamPrivacy()||!video_.Enabled();return CHIP_NO_ERROR;}
    bool HasAllocatedVideoStreams() override {return video_.Allocated(1)&&video_.Enabled();}
    bool HasAllocatedAudioStreams() override {return video_.AudioAllocated(VideoDelegate::kAudioStreamId)&&video_.Enabled();}
    CHIP_ERROR ValidateSFrameConfig(std::uint16_t,std::size_t) override {return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;}
    CHIP_ERROR IsUTCTimeNull(bool &isNull) override {isNull=true;return hooks_.utcIsNull?hooks_.utcIsNull(isNull):CHIP_ERROR_NOT_FOUND;}
private:
    enum class Kind {Description,Connected,Failed,SignalFailed};
    struct Event {std::uint64_t epoch;Kind kind;std::string sdp;bool offer=false;};
    struct Mailbox {std::mutex mutex;std::uint64_t epoch=0;bool failed=false;std::deque<Event>events;};
    static void Push(const std::weak_ptr<Mailbox>&weak,Event event){
        if(auto mail=weak.lock()){
            std::lock_guard<std::mutex> lock(mail->mutex);
            if(event.epoch!=mail->epoch)return;
            if(mail->events.size()>=6||event.sdp.size()>16384){mail->failed=true;return;}
            mail->events.push_back(std::move(event));
        }
    }
    bool Owner() const {return owner_==std::this_thread::get_id();}
    bool Match(std::uint16_t id) const {return session_&&target_.session==id;}
    std::uint64_t Now() const {return hooks_.nowMs?hooks_.nowMs():static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
    CHIP_ERROR Begin(const OfferRequestArgs &args,const std::string *offer,wp::WebRTCSessionStruct &out){
        lastStage_="owner-check";
        if(!Owner())return CHIP_ERROR_INCORRECT_STATE;
        if(session_)return CHIP_ERROR_BUSY;
        if(args.sessionId==65535||args.fabricIndex==chip::kUndefinedFabricIndex||!chip::IsOperationalNodeId(args.peerNodeId)||args.originatingEndpointId==65535)return CHIP_ERROR_INVALID_ARGUMENT;
        if(args.sFrameConfig.HasValue()||(args.iceTransportPolicy.HasValue()&&args.iceTransportPolicy.Value()!="all"))return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
        // Controllers (the HA Matter server) always pass ICE servers. Use at most
        // four plain STUN URLs; TURN needs credentials and LAN host candidates suffice.
        auto config=config_;
        if(args.iceServers.HasValue()){
            for(const auto &server:args.iceServers.Value()){
                auto url=server.URLs.begin();
                while(url.Next()&&config.iceServers.size()<4){
                    const auto &span=url.GetValue();const std::string text(span.data(),span.size());
                    if(text.size()>256||text.rfind("stun:",0)!=0)continue;
                    try{config.iceServers.emplace_back(text);}catch(...){}
                }
            }
        }
        if(!hooks_.signal||!hooks_.ensurePolling)return CHIP_ERROR_INCORRECT_STATE;
        lastStage_="validate-streams";auto videos=args.videoStreams,audios=args.audioStreams;
        auto err=ValidateStreamUsage(args.streamUsage,videos,audios);if(err!=CHIP_NO_ERROR)return err;
        lastStage_="acquire-reference";err=video_.AcquireReference();if(err!=CHIP_NO_ERROR)return err;
        referenced_=true;
        lastStage_="resume-source";err=video_.ResumeOwnSource();if(err!=CHIP_NO_ERROR){Close(false);return err;}
        audio_=!audios.Value().empty();
        if(audio_){
            lastStage_="acquire-audio";err=video_.AcquireAudioReference();
            if(err!=CHIP_NO_ERROR){audio_=false;Close(false);return err;}
            audioReferenced_=true;
        }
        target_={args.sessionId,chip::ScopedNodeId(args.peerNodeId,args.fabricIndex),args.originatingEndpointId,++epoch_};
        {std::lock_guard<std::mutex> lock(mail_->mutex);mail_->epoch=epoch_;mail_->events.clear();mail_->failed=false;}
        auto weak=std::weak_ptr<Mailbox>(mail_);auto epoch=epoch_;
        try {
            offering_=offer==nullptr;answered_=false;connected_=false;started_=lastFrame_=Now();
            lastStage_="create-peer";session_=std::make_unique<VideoPeer>(config,[weak,epoch](std::string sdp,bool isOffer){Push(weak,Event{epoch,Kind::Description,std::move(sdp),isOffer});},
                [weak,epoch](rtc::PeerConnection::State state){
                    if(state==rtc::PeerConnection::State::Connected)Push(weak,Event{epoch,Kind::Connected,{},{}});
                    if(state==rtc::PeerConnection::State::Failed||state==rtc::PeerConnection::State::Disconnected||state==rtc::PeerConnection::State::Closed)Push(weak,Event{epoch,Kind::Failed,{},{}});
                },hooks_.encoderProfile?hooks_.encoderProfile():kVideoProfile,audio_,
                audio_&&hooks_.talk&&hooks_.talkAllowed&&hooks_.talkAllowed()?hooks_.talk:VideoPeer::TalkCallback{});
            lastStage_="schedule-poll";err=hooks_.ensurePolling();if(err!=CHIP_NO_ERROR){Close(false);return err;}
            lastStage_="parse-sdp";
            if(offer)session_->Answer(*offer);else session_->Offer();
        }catch(const std::invalid_argument &error){
            const std::string reason=error.what();
            lastStage_=reason=="compatible Main H264 offer required"?"sdp-no-main-h264":
                reason=="video-only offer required"?"sdp-extra-media":"sdp-invalid";
            Close(false);return CHIP_ERROR_INVALID_ARGUMENT;
        }catch(...){lastStage_="sdp-failed";Close(false);return CHIP_ERROR_INVALID_ARGUMENT;}
        wp::WebRTCSessionStruct result;
        result.id=args.sessionId;result.peerNodeID=args.peerNodeId;result.peerEndpointID=args.originatingEndpointId;
        result.fabricIndex=args.fabricIndex;result.streamUsage=args.streamUsage;
        result.videoStreamID.SetNonNull(1);result.metadataEnabled=false;
        if(audio_)result.audioStreamID.SetNonNull(VideoDelegate::kAudioStreamId);else result.audioStreamID.SetNull();
        result.videoStreams.SetValue(chip::app::DataModel::List<const std::uint16_t>(videoIds_.data(),videoIds_.size()));
        result.audioStreams.SetValue(audio_?chip::app::DataModel::List<const std::uint16_t>(audioIds_.data(),audioIds_.size()):
            chip::app::DataModel::List<const std::uint16_t>());
        out=result;lastStage_="negotiating";return CHIP_NO_ERROR;
    }
    void Close(bool remove){
        const auto id=target_.session;bool existed=session_!=nullptr;
        if(existed&&hooks_.cancelSignal){try{hooks_.cancelSignal(target_);}catch(...){}}
        {std::lock_guard<std::mutex> lock(mail_->mutex);mail_->epoch=++epoch_;mail_->events.clear();mail_->failed=false;}
        if(session_){session_->Close();session_.reset();}
        if(audioReferenced_){video_.ReleaseAudioReference();audioReferenced_=false;}
        audio_=false;
        if(referenced_){video_.ReleaseReference();referenced_=false;video_.SuspendOwnSource();}
        connected_=false;
        if(remove&&existed&&cluster_)cluster_->RemoveSession(id);
    }
    VideoDelegate &video_;
    rtc::Configuration config_;
    Hooks hooks_;
    std::thread::id owner_;
    std::shared_ptr<Mailbox> mail_;
    std::unique_ptr<VideoPeer> session_;
    wp::WebRTCTransportProviderCluster *cluster_=nullptr;
    Target target_{};
    std::array<std::uint16_t,1> videoIds_{{1}},audioIds_{{VideoDelegate::kAudioStreamId}};
    std::uint64_t epoch_=0,started_=0,lastFrame_=0;
    bool offering_=false,answered_=false,connected_=false,referenced_=false,audio_=false,audioReferenced_=false;
    const char *lastStage_="idle";
};
}
