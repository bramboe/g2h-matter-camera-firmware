// Independent on-LAN Matter/CASE/WebRTC receiver. No camera media is saved.
#include "commands/common/Commands.h"
#include "commands/example/ExampleCredentialIssuerCommands.h"
#include "commands/clusters/ModelCommand.h"
#include "commands/icd/ICDCommand.h"
#include <app/clusters/webrtc-transport-requestor-server/WebRTCTransportRequestorCluster.h>
#include <app/server-cluster/ServerClusterInterfaceRegistry.h>
#include <data-model-providers/codegen/CodegenDataModelProvider.h>
#include <controller/InvokeInteraction.h>
#include <access/ProviderDeviceTypeResolver.h>
#include <access/examples/ExampleAccessControlDelegate.h>
#include <clusters/WebRTCTransportProvider/Commands.h>
#include <clusters/CameraAvStreamManagement/Commands.h>
#include <rtc/rtc.hpp>
#include <atomic>
#include <mutex>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fstream>

using namespace chip;
using namespace chip::app;
namespace requestor=Clusters::WebRTCTransportRequestor;
namespace provider=Clusters::WebRTCTransportProvider;

class CameraVideoProof final : public ModelCommand,public requestor::Delegate {
public:
    CameraVideoProof(CredentialIssuerCommands *issuer):ModelCommand("video",issuer){
        AddArguments();AddArgument("browser-directory",&browserDir_,"Private directory for an isolated recv-only browser proof.");AddArgument("local-ip",&localIp_,"Mac LAN IPv4 address used by the bounded test receiver.");
        AddArgument("frames",1,7200,&frameLimit_,"Number of decoded frames in the bounded receiver test (default20).");
        AddArgument("audio",0,1,&audio_,"Also allocate the camera microphone (Opus) and count received frames.");
        AddArgument("talk-file",&talkFile_,"Opus packets (u16 BE length + bytes) sent to the camera speaker in real time.");
    }
    bool NeedsOperationalAdvertising() override {return true;}
    System::Clock::Timeout GetWaitDuration() const override {return System::Clock::Seconds16(420);}
    CHIP_ERROR SendCommand(DeviceProxy *device,std::vector<EndpointId> endpoints) override {
        try {
        if(endpoints.size()!=1||endpoints[0]!=1||!localIp_.HasValue()||std::strlen(localIp_.Value())>15)return CHIP_ERROR_INVALID_ARGUMENT;
        target_=device->GetDeviceId();endpoint_=endpoints[0];exchange_=device->GetExchangeManager();session_.Grab(device->GetSecureSession().Value());
        // A controller accepting callback commands needs an initialized ACL.
        // Grant only this authenticated camera/fabric access to Requestor ep1.
        auto &acl=Access::GetAccessControl();
        ReturnErrorOnFailure(acl.Init(Access::Examples::GetAccessControlDelegate(),resolver_));accessInitialized_=true;
        Access::AccessControl::Entry entry;ReturnErrorOnFailure(acl.PrepareEntry(entry));
        ReturnErrorOnFailure(entry.SetAuthMode(Access::AuthMode::kCase));
        ReturnErrorOnFailure(entry.SetFabricIndex(CurrentCommissioner().GetFabricIndex()));
        ReturnErrorOnFailure(entry.SetPrivilege(Access::Privilege::kOperate));
        ReturnErrorOnFailure(entry.AddSubject(nullptr,target_));
        Access::AccessControl::Entry::Target target{};
        target.flags=target.kCluster|target.kEndpoint;target.cluster=requestor::Id;target.endpoint=1;
        ReturnErrorOnFailure(entry.AddTarget(nullptr,target));ReturnErrorOnFailure(acl.CreateEntry(nullptr,entry));
        requestor_.Create(1,*this);
        ReturnErrorOnFailure(CodegenDataModelProvider::Instance().Registry().Register(requestor_.Registration()));registered_=true;
        // H264 goes only through pipes/RAM to a decoder. stdout contains frame
        // checksums, never video bytes. Limit to twenty decoded frames.
        callbacks_->limit=frameLimit_.ValueOr(20);
        if(browserDir_.HasValue()){
            struct stat st{};
            if(lstat(browserDir_.Value(),&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&077))return CHIP_ERROR_INVALID_ARGUMENT;
            browserFd_=open(browserDir_.Value(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(browserFd_<0)return CHIP_ERROR_INVALID_ARGUMENT;
            offer_=ReadBrowserFile("offer.sdp",16384);
            if(offer_.empty())return CHIP_ERROR_INVALID_ARGUMENT;
            descriptionReady_=true;Allocate();
            ReturnErrorOnFailure(DeviceLayer::SystemLayer().StartTimer(System::Clock::Milliseconds32(50),Tick,this));timer_=true;
            return CHIP_NO_ERROR;
        }
        const std::string decoderCommand="/opt/homebrew/bin/ffmpeg -hide_banner -loglevel error -threads 1 -f h264 -i pipe:0 -an -frames:v "+std::to_string(callbacks_->limit)+" -f framemd5 pipe:1";
        decoder_=popen(decoderCommand.c_str(),"w");
        if(!decoder_)return CHIP_ERROR_INTERNAL;
        rtc::Configuration config;config.bindAddress=localIp_.Value();config.disableAutoNegotiation=true;
        config.enableIceTcp=false;config.portRangeBegin=60100;config.portRangeEnd=60199;
        peer_=std::make_shared<rtc::PeerConnection>(config);
        rtc::Description::Video video("video",rtc::Description::Direction::RecvOnly);
        video.addH264Codec(96,"profile-level-id=4d4028;packetization-mode=1;level-asymmetry-allowed=1");
        video.addH264Codec(98,"profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=1");
        track_=peer_->addTrack(video);
        auto depacketizer=std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::LongStartSequence);
        depacketizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        track_->setMediaHandler(depacketizer);
        if(audio_.ValueOr(0)&&!SetupAudio())return CHIP_ERROR_INVALID_ARGUMENT;
        track_->onFrame([state=callbacks_](rtc::binary bytes,rtc::FrameInfo){
            if(state->stopping||state->frames>=state->limit)return;
            // An incomplete RTP fragment group may produce no access unit.
            // Record network loss and continue; it is not an oversized frame.
            if(bytes.empty()){++state->emptyFrames;return;}
            if(bytes.size()>520*1024){state->failureReason=1;state->failed=true;return;}
            std::lock_guard<std::mutex> lock(state->mediaMutex);
            if(state->stopping||state->frames>=state->limit||!state->decoder)return;
            if(fwrite(bytes.data(),1,bytes.size(),state->decoder)!=bytes.size()){state->failureReason=2;state->failed=true;return;}
            ++state->frames;
        });
        peer_->onStateChange([mail=callbacks_](rtc::PeerConnection::State state){
            if(state==rtc::PeerConnection::State::Connected)mail->connected=true;
            if(!mail->stopping&&state==rtc::PeerConnection::State::Failed){mail->failureReason=3;mail->failed=true;}
        });
        peer_->onGatheringStateChange([mail=callbacks_,weak=std::weak_ptr<rtc::PeerConnection>(peer_)](rtc::PeerConnection::GatheringState state){
            if(state!=rtc::PeerConnection::GatheringState::Complete||mail->stopping)return;
            try{auto peer=weak.lock();if(!peer)return;auto description=peer->localDescription();if(!description){mail->failed=true;return;}
                auto text=std::string(*description);if(text.size()>16384){mail->failed=true;return;}
                std::lock_guard<std::mutex> lock(mail->descriptionMutex);mail->offer=std::move(text);mail->descriptionReady=true;
            }catch(...){mail->failed=true;}
        });
        Allocate();
        ReturnErrorOnFailure(DeviceLayer::SystemLayer().StartTimer(System::Clock::Milliseconds32(50),Tick,this));timer_=true;
        return CHIP_NO_ERROR;
        }catch(...){return CHIP_ERROR_INTERNAL;}
    }
    CHIP_ERROR HandleOffer(const requestor::WebRTCSessionStruct &,const OfferArgs &) override {return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;}
    CHIP_ERROR HandleAnswer(const requestor::WebRTCSessionStruct &session,const std::string &sdp) override {
        if(session.id!=sessionId_||session.peerNodeID!=target_||sdp.size()>16384||answered_)return CHIP_ERROR_INVALID_ARGUMENT;
        try {
            if(browserFd_>=0){
                int fd=openat(browserFd_,"answer.sdp.part",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
                if(fd<0)return CHIP_ERROR_INTERNAL;
                size_t at=0;while(at<sdp.size()){ssize_t n=write(fd,sdp.data()+at,sdp.size()-at);if(n<=0){close(fd);return CHIP_ERROR_INTERNAL;}at+=size_t(n);}close(fd);
                if(renameat(browserFd_,"answer.sdp.part",browserFd_,"answer.sdp"))return CHIP_ERROR_INTERNAL;
            }else peer_->setRemoteDescription(rtc::Description(sdp,rtc::Description::Type::Answer));
            answered_=true;answeredAt_=NowMs();return CHIP_NO_ERROR;
        }
        catch(...){callbacks_->failureReason=4;failed_=true;return CHIP_ERROR_INVALID_ARGUMENT;}
    }
    CHIP_ERROR HandleICECandidates(const requestor::WebRTCSessionStruct &session,const std::vector<requestor::ICECandidateStruct> &candidates) override {
        if(session.id!=sessionId_||session.peerNodeID!=target_||candidates.size()>16)return CHIP_ERROR_INVALID_ARGUMENT;
        if(browserFd_>=0)return CHIP_NO_ERROR; // The gathered answer carries candidates in this test.
        try{for(const auto &candidate:candidates){
            if(candidate.SDPMid.IsNull()||candidate.candidate.size()>2048||candidate.SDPMid.Value().size()>32)return CHIP_ERROR_INVALID_ARGUMENT;
            peer_->addRemoteCandidate(rtc::Candidate(std::string(candidate.candidate.data(),candidate.candidate.size()),std::string(candidate.SDPMid.Value().data(),candidate.SDPMid.Value().size())));
        }return CHIP_NO_ERROR;}catch(...){return CHIP_ERROR_INVALID_ARGUMENT;}
    }
    CHIP_ERROR HandleEnd(const requestor::WebRTCSessionStruct &,requestor::WebRTCEndReasonEnum) override {if(frames_<callbacks_->limit){callbacks_->failureReason=5;failed_=true;}return CHIP_NO_ERROR;}
    void Shutdown() override {
        stopping_=true;
        if(timer_){DeviceLayer::SystemLayer().CancelTimer(Tick,this);timer_=false;}
        auto cancel=std::move(cancel_);cancel_={};if(cancel)cancel();
        if(peer_){peer_->close();peer_.reset();track_.reset();audioTrack_.reset();}
        {std::lock_guard<std::mutex> lock(mediaMutex_);if(decoder_){pclose(decoder_);decoder_=nullptr;}}
        if(registered_){(void)CodegenDataModelProvider::Instance().Registry().Unregister(&requestor_.Cluster());registered_=false;}
        if(accessInitialized_){Access::GetAccessControl().Finish();accessInitialized_=false;}
        if(browserFd_>=0){close(browserFd_);browserFd_=-1;}
        session_.Release();ModelCommand::Shutdown();
    }
private:
    std::string ReadBrowserFile(const char *name,size_t limit){
        int fd=openat(browserFd_,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return {};
        struct stat st{};if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&077)||st.st_size<1||size_t(st.st_size)>limit){close(fd);return {};}
        std::string value(size_t(st.st_size),'\0');size_t at=0;while(at<value.size()){ssize_t n=read(fd,value.data()+at,value.size()-at);if(n<=0){close(fd);return {};}at+=size_t(n);}close(fd);return value;
    }
    void Allocate(){
        namespace av=chip::app::Clusters::CameraAvStreamManagement;
        av::Commands::VideoStreamAllocate::Type command{};command.streamUsage=chip::app::Clusters::Globals::StreamUsageEnum::kLiveView;command.videoCodec=av::VideoCodecEnum::kH264;
        command.minFrameRate=1;command.maxFrameRate=20;command.minResolution={1280,720};command.maxResolution={1280,720};
        command.minBitRate=1;command.maxBitRate=UINT32_MAX;command.keyFrameInterval=0;
        auto ok=[this](const ConcreteCommandPath&,const StatusIB&,const auto &response){
            cancel_={};if(response.videoStreamID!=1){Finish(CHIP_ERROR_INVALID_ARGUMENT);return;}
            if(audio_.ValueOr(0)){AllocateAudio();return;}
            allocated_=true;if(browserFd_<0)peer_->setLocalDescription(rtc::Description::Type::Offer);
        };
        auto bad=[this](CHIP_ERROR error){cancel_={};Finish(error);};
        auto error=Controller::InvokeCommandRequest(exchange_,session_.Get().Value(),endpoint_,command,ok,bad,NullOptional,MakeOptional(System::Clock::Timeout(10000)),&cancel_,true);
        if(error!=CHIP_NO_ERROR)Finish(error);
    }
    // Opus mono 48 kHz, the camera's advertised microphone; the camera returns stream 3.
    void AllocateAudio(){
        namespace av=chip::app::Clusters::CameraAvStreamManagement;
        av::Commands::AudioStreamAllocate::Type command{};command.streamUsage=chip::app::Clusters::Globals::StreamUsageEnum::kLiveView;
        command.audioCodec=av::AudioCodecEnum::kOpus;command.channelCount=1;command.sampleRate=48000;command.bitRate=64000;command.bitDepth=16;
        auto ok=[this](const ConcreteCommandPath&,const StatusIB&,const auto &response){
            cancel_={};if(response.audioStreamID!=audioIds_[0]){Finish(CHIP_ERROR_INVALID_ARGUMENT);return;}
            allocated_=true;peer_->setLocalDescription(rtc::Description::Type::Offer);
        };
        auto bad=[this](CHIP_ERROR error){cancel_={};callbacks_->failureReason=9;Finish(error);};
        auto error=Controller::InvokeCommandRequest(exchange_,session_.Get().Value(),endpoint_,command,ok,bad,NullOptional,MakeOptional(System::Clock::Timeout(10000)),&cancel_,true);
        if(error!=CHIP_NO_ERROR)Finish(error);
    }
    // Receive-only audio, or send-receive when a talk file is given.
    bool SetupAudio(){
        if(talkFile_.HasValue()){
            std::ifstream f(talkFile_.Value(),std::ios::binary);std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)),{});
            for(std::size_t at=0;at+2<=raw.size();){
                const std::size_t n=(std::size_t(raw[at])<<8)|raw[at+1];at+=2;
                if(!n||n>1275||at+n>raw.size()||talk_.size()>=500)return false;
                talk_.emplace_back(raw.begin()+long(at),raw.begin()+long(at+n));at+=n;
            }
            if(talk_.empty())return false;
        }
        const bool talk=!talk_.empty();
        rtc::Description::Audio audio("audio",talk?rtc::Description::Direction::SendRecv:rtc::Description::Direction::RecvOnly);
        audio.addOpusCodec(111,std::string("minptime=10;useinbandfec=1"));
        if(talk)audio.addSSRC(99,"g2h-test","g2h-test","audio");
        audioTrack_=peer_->addTrack(audio);
        auto depacketizer=std::make_shared<rtc::OpusRtpDepacketizer>();depacketizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        if(talk){
            auto rtp=std::make_shared<rtc::RtpPacketizationConfig>(99,"g2h-test",111,rtc::OpusRtpPacketizer::DefaultClockRate);rtp->mid="audio";
            auto packetizer=std::make_shared<rtc::OpusRtpPacketizer>(rtp);packetizer->addToChain(depacketizer);audioTrack_->setMediaHandler(packetizer);
        }else audioTrack_->setMediaHandler(depacketizer);
        audioTrack_->onFrame([state=callbacks_](rtc::binary bytes,rtc::FrameInfo){if(bytes.size()>2)++state->audioFrames;});
        return true;
    }
    void SendTalk(){
        if(talk_.empty()||talkSent_>=talk_.size()||!connected_||!audioTrack_||!audioTrack_->isOpen())return;
        const auto now=NowMs();if(!talkStart_)talkStart_=now;
        const std::size_t due=std::min<std::size_t>(talk_.size(),std::size_t((now-talkStart_)/20)+1);
        while(talkSent_<due){
            const auto &packet=talk_[talkSent_];
            try{audioTrack_->sendFrame(rtc::binary(reinterpret_cast<const std::byte*>(packet.data()),reinterpret_cast<const std::byte*>(packet.data())+packet.size()),
                rtc::FrameInfo(std::uint32_t(talkSent_*960)));}catch(...){callbacks_->failureReason=10;failed_=true;return;}
            ++talkSent_;
        }
        if(talkSent_==talk_.size())talkDoneAt_=now;
    }
    bool AudioDone() const {
        if(!audio_.ValueOr(0))return true;
        if(callbacks_->audioFrames<50)return false; // 1 s of 20 ms microphone frames
        return talk_.empty()||(talkDoneAt_&&NowMs()-talkDoneAt_>=1500); // let the last chunk play
    }
    void SendOffer(){
        provider::Commands::ProvideOffer::Type command;
        command.webRTCSessionID.SetNull();command.streamUsage=requestor::StreamUsageEnum::kLiveView;command.originatingEndpointID=1;
        command.videoStreams.SetValue(DataModel::List<const uint16_t>(videoIds_,1));
        if(audio_.ValueOr(0))command.audioStreams.SetValue(DataModel::List<const uint16_t>(audioIds_,1));
        // No audio feature: omit AudioStreams rather than requesting an audio
        // allocation with an empty array (the SDK validates allocations first).
        command.sdp=CharSpan(offer_.data(),offer_.size());
        auto ok=[this](const ConcreteCommandPath&,const StatusIB&,const auto &response){
            cancel_={};sessionId_=response.webRTCSessionID;sessionCreated_=true;sessionAt_=progressAt_=NowMs();
            requestor::WebRTCSessionStruct session;session.id=sessionId_;session.peerNodeID=target_;session.peerEndpointID=endpoint_;
            session.fabricIndex=CurrentCommissioner().GetFabricIndex();session.streamUsage=requestor::StreamUsageEnum::kLiveView;
            session.videoStreamID.SetNonNull(1);
            if(audio_.ValueOr(0))session.audioStreamID.SetNonNull(audioIds_[0]);else session.audioStreamID.SetNull();
            requestor_.Cluster().UpsertSession(session);
        };
        auto bad=[this](CHIP_ERROR err){cancel_={};Finish(err);};
        auto err=Controller::InvokeCommandRequest(exchange_,session_.Get().Value(),endpoint_,command,ok,bad,NullOptional,
            MakeOptional(System::Clock::Timeout(10000)),&cancel_,true);
        if(err!=CHIP_NO_ERROR)Finish(err);
    }
    void End(){
        ending_=true;provider::Commands::EndSession::Type command;command.webRTCSessionID=sessionId_;command.reason=requestor::WebRTCEndReasonEnum::kUserHangup;
        auto ok=[this](const ConcreteCommandPath&,const StatusIB&,const auto &){cancel_={};Finish(CHIP_NO_ERROR);};
        auto bad=[this](CHIP_ERROR err){cancel_={};Finish(err);};
        auto err=Controller::InvokeCommandRequest(exchange_,session_.Get().Value(),endpoint_,command,ok,bad,NullOptional,
            MakeOptional(System::Clock::Timeout(10000)),&cancel_,true);
        if(err!=CHIP_NO_ERROR)Finish(err);
    }
    void Finish(CHIP_ERROR err){
        if(finished_)return;finished_=true;stopping_=true;
        if(peer_)peer_->close();
        int decoded=browserFd_>=0?0:-1;{std::lock_guard<std::mutex> lock(mediaMutex_);if(decoder_){decoded=pclose(decoder_);decoder_=nullptr;}}
        if(err==CHIP_NO_ERROR&&(!answered_||!connected_||frames_<callbacks_->limit||decoded!=0||!AudioDone()))err=CHIP_ERROR_INTERNAL;
        if(audio_.ValueOr(0))std::printf("G2H_MATTER_AUDIO {\"received_opus_frames\":%u,\"talk_packets\":%zu,\"talk_packets_sent\":%zu,\"success\":%s}\n",
            callbacks_->audioFrames.load(),talk_.size(),talkSent_,err==CHIP_NO_ERROR?"true":"false");
        if(browserFd_>=0)std::printf("G2H_BROWSER_MATTER_SIGNALING {\"case_answer_received\":%s,\"browser_proof_marker_verified\":%s,\"success\":%s}\n",answered_?"true":"false",connected_?"true":"false",err==CHIP_NO_ERROR?"true":"false");
        else std::printf("G2H_EXTERNAL_VIDEO {\"case_signaling_answer_received\":%s,\"ice_dtls_srtp_connected\":%s,\"received_h264_frames\":%u,\"dropped_empty_frames\":%u,\"failure_reason\":%d,\"decoder_exit\":%d,\"media_saved\":false,\"success\":%s}\n",answered_?"true":"false",connected_?"true":"false",frames_.load(),callbacks_->emptyFrames.load(),callbacks_->failureReason.load(),decoded,err==CHIP_NO_ERROR?"true":"false");
        SetCommandExitStatus(err);
    }
    static void Tick(System::Layer*,void *context){
        auto &self=*static_cast<CameraVideoProof*>(context);self.timer_=false;
        if(self.finished_)return;
        if(self.browserFd_>=0&&self.ReadBrowserFile("done",16)=="G2H_BROWSER_OK\n"){
            self.callbacks_->connected=true;self.callbacks_->frames=self.callbacks_->limit;
        }
        if(self.failed_){self.Finish(CHIP_ERROR_INTERNAL);return;}
        if(!self.offered_&&self.descriptionReady_&&self.allocated_){
            self.offered_=true;
            {std::lock_guard<std::mutex> lock(self.descriptionMutex_);self.SendOffer();}
        }
        self.SendTalk();
        if(self.frames_>=self.callbacks_->limit&&self.AudioDone()&&self.sessionCreated_&&!self.ending_)self.End();
        // The camera ends a stalled session only locally, so bound each phase
        // here: 6 no Answer, 7 answered but no ICE/DTLS, 8 media stalled.
        if(self.sessionCreated_&&!self.ending_&&!self.finished_){
            const auto now=NowMs();const unsigned frames=self.frames_;
            if(frames!=self.progressFrames_){self.progressFrames_=frames;self.progressAt_=now;}
            int reason=0;
            if(!self.answered_&&now-self.sessionAt_>=33000)reason=6;
            else if(self.browserFd_<0&&self.answered_&&!self.connected_&&now-self.answeredAt_>=20000)reason=7;
            else if(self.browserFd_<0&&self.connected_&&now-self.progressAt_>=10000)reason=8;
            if(reason){self.callbacks_->failureReason=reason;self.End();}
        }
        if(self.finished_)return;
        auto err=DeviceLayer::SystemLayer().StartTimer(System::Clock::Milliseconds32(50),Tick,&self);
        if(err!=CHIP_NO_ERROR)self.Finish(err);else self.timer_=true;
    }
    Optional<char*> localIp_,browserDir_;
    int browserFd_=-1;
    Optional<uint16_t> frameLimit_;
    Access::DynamicProviderDeviceTypeResolver resolver_{[]{return static_cast<DataModel::Provider*>(&CodegenDataModelProvider::Instance());}};
    Messaging::ExchangeManager *exchange_=nullptr;SessionHolder session_;
    NodeId target_=0;EndpointId endpoint_=1;uint16_t sessionId_=0;const uint16_t videoIds_[1]={1};
    LazyRegisteredServerCluster<requestor::WebRTCTransportRequestorCluster> requestor_;
    std::shared_ptr<rtc::PeerConnection> peer_;std::shared_ptr<rtc::Track> track_;
    struct CallbackState {
        FILE *decoder=nullptr;unsigned limit=20;std::mutex descriptionMutex,mediaMutex;std::string offer;
        std::atomic<bool> descriptionReady{false},connected{false},failed{false},stopping{false};std::atomic<unsigned> frames{0},emptyFrames{0},audioFrames{0};std::atomic<int> failureReason{0};
    };
    std::shared_ptr<CallbackState> callbacks_=std::make_shared<CallbackState>();
    FILE *&decoder_=callbacks_->decoder;
    std::mutex &descriptionMutex_=callbacks_->descriptionMutex,&mediaMutex_=callbacks_->mediaMutex;
    std::string &offer_=callbacks_->offer;
    std::atomic<bool> &descriptionReady_=callbacks_->descriptionReady,&connected_=callbacks_->connected,&failed_=callbacks_->failed,&stopping_=callbacks_->stopping;
    std::atomic<unsigned> &frames_=callbacks_->frames;
    static uint64_t NowMs(){return System::SystemClock().GetMonotonicMilliseconds64().count();}
    uint64_t sessionAt_=0,answeredAt_=0,progressAt_=0;unsigned progressFrames_=0;
    Optional<uint16_t> audio_;Optional<char*> talkFile_;const uint16_t audioIds_[1]={3};
    std::shared_ptr<rtc::Track> audioTrack_;std::vector<std::vector<uint8_t>> talk_;std::size_t talkSent_=0;uint64_t talkStart_=0,talkDoneAt_=0;
    bool allocated_=false;
    bool registered_=false,accessInitialized_=false,timer_=false,offered_=false,sessionCreated_=false,answered_=false,ending_=false,finished_=false;
    Controller::Internal::InvokeCancelFn cancel_;
};
#include "g2h_snapshot_controller.hpp"
int main(int argc,char **argv){
    std::signal(SIGPIPE,SIG_IGN);
    ExampleCredentialIssuerCommands issuer;Commands commands;
    registerCommandsICD(commands,&issuer);
    commands_list list={make_unique<CameraVideoProof>(&issuer),make_unique<CameraSnapshotProof>(&issuer)};
    commands.RegisterCommandSet("g2h",list,"Isolated real Matter/CASE/WebRTC camera proof.");
    return commands.Run(argc,argv);
}
