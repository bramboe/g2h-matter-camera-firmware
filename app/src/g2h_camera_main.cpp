// Experimental Linux camera endpoint. Installation/commissioning is a separate
// step: this executable must never replace the vendor camera or radio service.
#include "g2h_video_source.hpp"
#include "g2h_capture_pause.hpp"
#include "g2h_audio_source.hpp"
#include "g2h_snapshot_codec.hpp"
#include "g2h_snapshot_worker.hpp"
#include "g2h_matter_signaling.hpp"
#include "g2h_identity.hpp"
#include "g2h_thread_cluster.hpp"
#include "g2h_motion_cluster.hpp"
#include "g2h_motion_source.hpp"
#include "g2h_image_cluster.hpp"
#include "g2h_control_mode_cluster.hpp"
#include "g2h_video_control.hpp"
#include "g2h_video_endpoint.hpp"
#include "g2h_talkback.hpp"
#include <AppMain.h>
#include <Options.h>
#include <tls-certificate-management-instance.h>
#include <tls-client-management-instance.h>
#include <data-model-providers/codegen/Instance.h>
#include <data-model-providers/codegen/CodegenDataModelProvider.h>
#include <app/server-cluster/ServerClusterInterfaceRegistry.h>
#include <app/clusters/time-synchronization-server/time-synchronization-cluster.h>
#include <lib/support/TimeUtils.h>
#include <memory>
#include <fstream>
#include <csignal>
#include <sys/file.h>
#include <platform/KeyValueStoreManager.h>

namespace {
using namespace chip;
using namespace chip::app;
bool persistentMode=false,trialMode=false,commissionTrial=false;
volatile sig_atomic_t commissioningRequested=0;
bool windowEverOpened=false;
std::uint64_t sentFrames=0,sentAudio=0;
bool SaveAudioPreference(bool enabled){
    if(!persistentMode)return false;
    const uint8_t value=enabled?1:0;
    return DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put("g2h-audio-enabled",&value,sizeof(value))==CHIP_NO_ERROR;
}
bool SaveThreadPreference(bool enabled){
    if(!persistentMode)return false;
    const uint8_t value=enabled?1:0;
    return DeviceLayer::PersistedStorage::KeyValueStoreMgr().Put("g2h-thread-enabled",&value,sizeof(value))==CHIP_NO_ERROR;
}
class CameraRuntime {
public:
    CameraRuntime():video_({g2h::kVideoWidth,g2h::kVideoHeight,g2h::kVideoFps},[this]{return source_.Start();},[this]{source_.EnableFrames(false);},
        [this](unsigned quality,unsigned width,unsigned height,g2h::av::ImageSnapshot &image){
            // JPEG work runs on the snapshot worker. During a live session wait
            // well below the ~1.6 s frame queue; otherwise keep the old 2.3 s bound.
            const auto wait=rtc_.HasSession()?std::chrono::milliseconds(300):std::chrono::milliseconds(2300);
            std::vector<std::uint8_t> jpeg;
            if(source_.Faulted()||!snapshots_.Get(quality,wait,jpeg,width,height))return g2h::Status::ResourceExhausted;
            image.data=std::move(jpeg);image.imageRes={uint16_t(width),uint16_t(height)};image.imageCodec=g2h::av::ImageCodecEnum::kJpeg;
            return g2h::Status::Success;
        },[this](unsigned quality,unsigned width,unsigned height){snapshots_.Request(quality,width,height);},
        [this](unsigned bitRate){audio_.SetBitrate(bitRate);return audio_.Start(bitRate);}),
        rtc_(video_,rtc::Configuration{},Hooks()) {}
    CHIP_ERROR Init(){
        // The certificate dependency checker is real, but no TLS client
        // management cluster is exposed by this video-only model.
        ReturnErrorOnFailure(Clusters::TlsClientManagementCommandDelegate::GetInstance().Init(Server::GetInstance().GetPersistentStorage()));
        if(!snapshots_.Start())return CHIP_ERROR_NO_MEMORY;
        videoControlInstalled_=videoControl_.Available();ApplyCaptureGate();
        // Camera controls: the vendor value is the source of truth (HomeKit and the
        // Aqara app change it too); no Matter write reaches the vendor while syncing.
        syncing_=true;
        vendor_.ReadAll();
        // Native mutes are volatile across a camera boot. Restore only an
        // explicit global choice, keeping own audio closed until readback.
        if(persistentMode){
            uint8_t enabled=0;size_t length=0;
            const auto result=DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get("g2h-audio-enabled",&enabled,sizeof(enabled),&length);
            if(result==CHIP_NO_ERROR){
                VerifyOrReturnError(length==1&&enabled<=1,CHIP_ERROR_INCORRECT_STATE);
                audioPreference_=enabled!=0;
                VerifyOrReturnError(vendor_.WriteAudioMuted(enabled==0),CHIP_ERROR_INCORRECT_STATE);
            }else if(result!=CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND)return result;
        }
        if(!vendor_.Start())return CHIP_ERROR_NO_MEMORY;
        video_.attributeChanged=[this](AttributeId id){OnControlChanged(id);};
        bridge_.Attach(rtc_);
        provider_.Create(1,rtc_);
        auto &registry=CodegenDataModelProvider::Instance().Registry();
        radioCluster_.Create(radio_,SaveThreadPreference);
        ReturnErrorOnFailure(registry.Register(radioCluster_.Registration()));radioRegistered_=true;
        if(persistentMode){
            uint8_t enabled=0;size_t length=0;
            const auto result=DeviceLayer::PersistedStorage::KeyValueStoreMgr().Get("g2h-thread-enabled",&enabled,sizeof(enabled),&length);
            if(result==CHIP_NO_ERROR){
                VerifyOrReturnError(length==1&&enabled<=1,CHIP_ERROR_INCORRECT_STATE);
                VerifyOrReturnError(radio_.SetEnabled(enabled!=0),CHIP_ERROR_INCORRECT_STATE);
            }else if(result!=CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND)return result;
        }
        if(persistentMode||trialMode)radio_.Start();
        motionCluster_.Create(motion_.state);
        ReturnErrorOnFailure(registry.Register(motionCluster_.Registration()));motionRegistered_=true;
        if((persistentMode||trialMode)&&!motion_.Start())return CHIP_ERROR_NO_MEMORY;
        ReturnErrorOnFailure(registry.Register(provider_.Registration()));providerRegistered_=true;
        imageCluster_.Create(vendor_,std::function<void()>{},[this](bool enabled){return SetAudioEnabled(enabled);});
        ReturnErrorOnFailure(registry.Register(imageCluster_.Registration()));imageRegistered_=true;
        audioMode_.Create(1,"Camera audio",[this]()->std::optional<bool>{
            auto value=vendor_.AudioMuted();return value?std::optional<bool>(!*value):std::nullopt;
        },[this](bool enabled){return SetAudioEnabled(enabled);});
        ReturnErrorOnFailure(registry.Register(audioMode_.Registration()));audioModeRegistered_=true;
        threadMode_.Create(0,"Thread radio",[this]()->std::optional<bool>{
            auto control=radio_.ControlSnapshot();return control.pending?std::nullopt:control.observed;
        },[this](bool enabled){return SaveThreadPreference(enabled)&&radio_.SetEnabled(enabled);});
        ReturnErrorOnFailure(registry.Register(threadMode_.Registration()));threadModeRegistered_=true;
        if(videoControlInstalled_){
            ReturnErrorOnFailure(videoEndpoint_.Register());
            videoMode_.Create(2,"Camera video (restart)",[this]{return videoControl_.Observe();},[this](bool enabled){
                if(!captureRequested_&&videoControl_.Observe()==std::optional<bool>(enabled))return true;
                // Close every app copy synchronously before a backend request.
                captureRequested_=true;captureAllowed_=false;
                vendor_.SetVideoSettingsAllowed(false);
                video_.SetGlobalAllowed(false);snapshots_.SetGlobalAllowed(false);source_.SetGlobalAllowed(false);
                rtc_.Shutdown();
                return videoControl_.Request(enabled);
            });
            ReturnErrorOnFailure(registry.Register(videoMode_.Registration()));videoModeRegistered_=true;
        }
        rtc_.SetCluster(&provider_.Cluster());
        g2h::av::VideoSensorParamsStruct sensor{};sensor.sensorWidth=g2h::kVideoWidth;sensor.sensorHeight=g2h::kVideoHeight;sensor.maxFPS=g2h::kVideoFps;
        g2h::av::VideoResolutionStruct viewport{};viewport.width=g2h::kVideoWidth;viewport.height=g2h::kVideoHeight;
        // UINT32_MAX is an unconstrained admission upper bound, not measured
        // Wi-Fi throughput. No bitrate-changing capability is advertised.
        g2h::av::SnapshotCapabilitiesStruct snapshot{};snapshot.resolution={320,180};snapshot.maxFrameRate=1;
        snapshot.imageCodec=g2h::av::ImageCodecEnum::kJpeg;snapshot.requiresEncodedPixels=false;
        auto snapshotLarge=snapshot;snapshotLarge.resolution={640,360};
        // Microphone: the camera's AAC track, offered as Opus mono 48 kHz.
        static const g2h::av::AudioCodecEnum kMicCodecs[]={g2h::av::AudioCodecEnum::kOpus};
        static const std::uint32_t kMicRates[]={48000};static const std::uint8_t kMicDepths[]={16};
        g2h::av::AudioCapabilitiesStruct microphone{};microphone.maxNumberOfChannels=1;
        microphone.supportedCodecs=DataModel::List<const g2h::av::AudioCodecEnum>(kMicCodecs);
        microphone.supportedSampleRates=DataModel::List<const std::uint32_t>(kMicRates);
        microphone.supportedBitDepths=DataModel::List<const std::uint8_t>(kMicDepths);
        av_.Create(video_,1,BitFlags<g2h::av::Feature>(g2h::av::Feature::kVideo,g2h::av::Feature::kAudio,g2h::av::Feature::kSnapshot,
                g2h::av::Feature::kNightVision,g2h::av::Feature::kHighDynamicRange,g2h::av::Feature::kSpeaker,g2h::av::Feature::kPrivacy),
            BitFlags<g2h::av::OptionalAttribute>(g2h::av::OptionalAttribute::kStatusLightEnabled),
            1,g2h::kVideoWidth*g2h::kVideoHeight*g2h::kVideoFps,sensor,true /*night vision uses IR*/,viewport,std::vector<g2h::av::RateDistortionTradeOffStruct>{},
            // Speaker: Opus in, played as short AAC chunks by the vendor (≈0.5–1 s): half duplex.
            2u*1024u*1024u,microphone,microphone,
            g2h::av::TwoWayTalkSupportTypeEnum::kHalfDuplex,std::vector<g2h::av::SnapshotCapabilitiesStruct>{snapshot,snapshotLarge},
            UINT32_MAX,std::vector<g2h::av::StreamUsageEnum>{g2h::av::StreamUsageEnum::kLiveView},
            std::vector<g2h::av::StreamUsageEnum>{g2h::av::StreamUsageEnum::kLiveView});
        ReturnErrorOnFailure(registry.Register(av_.Registration()));avRegistered_=true;
        // SDK dimensions are x2-x1/y2-y1 (exclusive upper edge).
        Clusters::Globals::Structs::ViewportStruct::Type view{};view.x2=g2h::kVideoWidth;view.y2=g2h::kVideoHeight;
        ReturnErrorOnFailure(av_.Cluster().SetViewport(view));
        ReturnErrorOnFailure(av_.Cluster().SetCurrentFrameRate(20));
        ReturnErrorOnFailure(av_.Cluster().Init());
        talk_.SetMuted(av_.Cluster().GetSpeakerMuted()||av_.Cluster().GetSoftLivestreamPrivacyModeEnabled()||vendor_.Value(g2h::Vendor::SpeakerMuted).value_or(1)!=0);
        video_.SetLivestreamPrivacy(av_.Cluster().GetSoftLivestreamPrivacyModeEnabled());
        for(std::size_t i=0;i<g2h::VendorControl::kCount;++i)SyncFromVendor(g2h::Vendor(i));
        syncing_=false;
        if(video_.Faulted())return CHIP_ERROR_INCORRECT_STATE;
        ReturnErrorOnFailure(DeviceLayer::SystemLayer().StartTimer(System::Clock::Milliseconds32(20),Tick,this));
        timer_=true;return CHIP_NO_ERROR;
    }
    ~CameraRuntime(){
        if(timer_)DeviceLayer::SystemLayer().CancelTimer(Tick,this);
        motion_.Stop();radio_.Stop();rtc_.Shutdown();bridge_.Stop();snapshots_.Stop();audio_.Stop();talk_.Stop();vendor_.Stop();source_.Stop();rtc_.SetCluster(nullptr);
        auto &registry=CodegenDataModelProvider::Instance().Registry();
        if(audioModeRegistered_)(void)registry.Unregister(&audioMode_.Cluster());
        if(threadModeRegistered_)(void)registry.Unregister(&threadMode_.Cluster());
        if(videoModeRegistered_)(void)registry.Unregister(&videoMode_.Cluster());
        videoEndpoint_.Clear();
        if(motionRegistered_){(void)registry.Unregister(&motionCluster_.Cluster());motionRegistered_=false;}
        if(radioRegistered_){(void)registry.Unregister(&radioCluster_.Cluster());radioRegistered_=false;}
        if(providerRegistered_){(void)registry.Unregister(&provider_.Cluster());providerRegistered_=false;}
        if(avRegistered_){(void)registry.Unregister(&av_.Cluster());avRegistered_=false;}
        if(imageRegistered_){(void)registry.Unregister(&imageCluster_.Cluster());imageRegistered_=false;}
    }
private:
    bool SetAudioEnabled(bool enabled){
        if(!SaveAudioPreference(enabled))return false;
        audioPreference_=enabled;
        talk_.SetMuted(true);audio_.EnableFrames(false);
        return vendor_.WriteAudioMuted(!enabled);
    }
    // The private marker gates our copies only; it does not establish capture
    // state. VideoControl independently verifies the preference and hardware.
    void ApplyCaptureGate(){
        const auto now=Clock::now();
        if(captureRequested_||capturePause_.Paused())captureReleaseAt_=now+std::chrono::milliseconds(300);
        const bool allowed=now>=captureReleaseAt_;
        vendor_.SetVideoSettingsAllowed(allowed&&(!videoControlInstalled_||videoControl_.Observe()==std::optional<bool>(true)));
        if(allowed==captureAllowed_)return;
        if(!allowed){video_.SetGlobalAllowed(false);snapshots_.SetGlobalAllowed(false);source_.SetGlobalAllowed(false);}
        else{source_.SetGlobalAllowed(true);snapshots_.SetGlobalAllowed(true);video_.SetGlobalAllowed(true);}
        captureAllowed_=allowed;
    }
    // Matter → camera, for attributes written by a controller.
    void OnControlChanged(AttributeId id){
        if(syncing_||!avRegistered_)return;
        namespace attr=g2h::av::Attributes;auto &c=av_.Cluster();
        if(id==attr::NightVision::Id){if(!vendor_.Write(g2h::Vendor::NightVision,int(to_underlying(c.GetNightVision())))){syncing_=true;SyncFromVendor(g2h::Vendor::NightVision);syncing_=false;}}
        else if(id==attr::StatusLightEnabled::Id)(void)vendor_.Write(g2h::Vendor::StatusLight,c.GetStatusLightEnabled()?1:0);
        else if(id==attr::HDRModeEnabled::Id){if(!vendor_.Write(g2h::Vendor::Wdr,c.GetHDRModeEnabled()?1:0)){syncing_=true;SyncFromVendor(g2h::Vendor::Wdr);syncing_=false;}}
        else if(id==attr::SpeakerVolumeLevel::Id)(void)vendor_.Write(g2h::Vendor::SpeakerVolume,(int(c.GetSpeakerVolumeLevel())*100+127)/254);
        else if(id==attr::SpeakerMuted::Id)talk_.SetMuted(c.GetSpeakerMuted()||c.GetSoftLivestreamPrivacyModeEnabled()||vendor_.Value(g2h::Vendor::SpeakerMuted).value_or(1)!=0);
        // App-level privacy: stop sending video/audio over Matter; vendor stream untouched.
        else if(id==attr::SoftLivestreamPrivacyModeEnabled::Id){
            video_.SetLivestreamPrivacy(c.GetSoftLivestreamPrivacyModeEnabled());
            talk_.SetMuted(c.GetSpeakerMuted()||c.GetSoftLivestreamPrivacyModeEnabled()||vendor_.Value(g2h::Vendor::SpeakerMuted).value_or(1)!=0);
        }
    }
    // Camera → Matter. Vendor night vision 0/1/2 equals TriStateAutoEnum Off/On/Auto (verified 2026-10-22).
    void SyncFromVendor(g2h::Vendor which){
        const auto value=vendor_.Value(which);if(!value)return;
        auto &c=av_.Cluster();
        switch(which){
            case g2h::Vendor::NightVision:(void)c.SetNightVision(g2h::av::TriStateAutoEnum(*value));break;
            case g2h::Vendor::StatusLight:(void)c.SetStatusLightEnabled(*value!=0);break;
            case g2h::Vendor::Wdr:(void)c.SetHDRModeEnabled(*value!=0);break;
            case g2h::Vendor::SpeakerVolume: // vendor 0..100 ↔ Matter 0..254; also the G2H attribute
                (void)c.SetSpeakerVolumeLevel(std::uint8_t((*value*254+50)/100));
                if(imageRegistered_)imageCluster_.Cluster().Changed(which);break;
            default:if(imageRegistered_)imageCluster_.Cluster().Changed(which);break;
        }
    }
    g2h::WebRtcDelegate::Hooks Hooks(){
        g2h::WebRtcDelegate::Hooks hooks;
        hooks.encoderProfile=[this]{return source_.Profile();};
        hooks.talkAllowed=[this]{return audioPreference_!=std::optional<bool>(false)&&avRegistered_&&!av_.Cluster().GetSpeakerMuted()&&!av_.Cluster().GetSoftLivestreamPrivacyModeEnabled()&&vendor_.Value(g2h::Vendor::SpeakerMuted).value_or(1)==0&&talk_.Start();};
        hooks.talk=[this](const std::uint8_t *opus,std::size_t size){talk_.PushOpus(opus,size);};
        hooks.ensurePolling=[this]{return bridge_.EnsurePolling();};
        hooks.signal=[this](const auto &target,const std::string &sdp,bool offer,std::function<void()> failed){
            return bridge_.Send(target,sdp,offer,std::move(failed));};
        hooks.cancelSignal=[this](const auto &target){bridge_.Cancel(target);};
        hooks.utcIsNull=[](bool &isNull){
            isNull=true;
            if(Clusters::TimeSynchronization::TimeSynchronizationServer::Instance().GetGranularity()==
                Clusters::TimeSynchronization::GranularityEnum::kNoTimeGranularity)return CHIP_NO_ERROR;
            System::Clock::Microseconds64 utc;uint64_t epoch;
            if(System::SystemClock().GetClock_RealTime(utc)==CHIP_NO_ERROR&&UnixEpochToChipEpochMicros(utc.count(),epoch))isNull=false;
            return CHIP_NO_ERROR;
        };return hooks;
    }
    static void Tick(System::Layer*,void *context){
        auto &self=*static_cast<CameraRuntime*>(context);self.timer_=false;
        try{
            self.ApplyCaptureGate();
            self.motionCluster_.Cluster().Refresh();
            self.audioMode_.Cluster().Refresh();self.threadMode_.Cluster().Refresh();
            if(self.videoModeRegistered_)self.videoMode_.Cluster().Refresh();
            auto &window=Server::GetInstance().GetCommissioningWindowManager();
            if(commissioningRequested){
                commissioningRequested=0;
                if(!window.IsCommissioningWindowOpen())
                    (void)window.OpenBasicCommissioningWindow(System::Clock::Seconds32(900),CommissioningWindowAdvertisement::kDnssdOnly);
            }
            windowEverOpened|=window.IsCommissioningWindowOpen();
            // A failed ingest ends the session only; video stays enabled and the next request
            // reopens the source (rate-limited in VideoSource). It used to disable video for good.
            if(self.source_.Faulted()){if(!self.sourceLost_){self.rtc_.Shutdown();self.video_.SourceLost();self.sourceLost_=true;}}
            else self.sourceLost_=false;
            const bool ready=self.rtc_.Ready();
            self.source_.EnableFrames(ready);
            g2h::H264Rtp::Frame frame;
            for(unsigned n=0;ready&&n<8&&self.source_.Pop(frame);++n){
                if(self.rtc_.SendFrame(frame.annexB,frame.timestamp)!=CHIP_NO_ERROR)break;
                ++sentFrames;
            }
            // Settings changed on the camera itself (HomeKit, Aqara app) since the last poll.
            const auto changed=self.vendor_.TakeChanges();
            for(std::size_t i=0;i<changed.size();++i)if(changed[i]){self.syncing_=true;self.SyncFromVendor(g2h::Vendor(i));self.syncing_=false;}
            self.talk_.SetMuted(self.audioPreference_==std::optional<bool>(false)||self.av_.Cluster().GetSpeakerMuted()||self.av_.Cluster().GetSoftLivestreamPrivacyModeEnabled()||self.vendor_.Value(g2h::Vendor::SpeakerMuted).value_or(1)!=0);
            // MicrophoneMuted is honoured by sending no audio at all.
            const bool audio=self.audioPreference_!=std::optional<bool>(false)&&self.rtc_.AudioReady()&&!self.av_.Cluster().GetMicrophoneMuted()&&self.vendor_.Value(g2h::Vendor::MicrophoneMuted).value_or(1)==0;
            self.audio_.EnableFrames(audio);
            g2h::AudioSource::Packet sound;
            for(unsigned n=0;audio&&n<8&&self.audio_.Pop(sound);++n)
                if(self.rtc_.SendAudio(sound.opus,sound.timestamp)!=CHIP_NO_ERROR)break;else ++sentAudio;
            // Wall-clock cadence: snapshot capture blocks this thread, so a
            // tick count is not a time base. Session changes write at once.
            const auto now=Clock::now();const bool session=self.rtc_.HasSession();
            const bool periodic=now-self.statusAt_>=std::chrono::seconds(5);
            if(periodic||session!=self.statusSession_){
                self.statusAt_=now;self.statusSession_=session;
                if(periodic)self.radioCluster_.Cluster().Refresh();
                const char *temp="/tmp/g2h-matter-camera-status.tmp";
                int fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
                if(fd>=0){
                    if(FILE *f=fdopen(fd,"w")){
                        auto radio=self.radio_.Snapshot();
                        auto motion=self.motion_.state.Snapshot();auto motionValue=motion.Value();
                        std::fprintf(f,"{\"pid\":%ld,\"ready\":true,\"fabrics\":%u,\"commissioning_window_open\":%s,\"video_session\":%s,\"frames_sent\":%llu,\"video_enabled\":%s,\"source_fault\":%s,\"delegate_owner_thread\":%s,\"last_video_stage\":\"%s\",\"thread_fresh\":%s,\"thread_role\":\"%s\",\"monotonic_ms\":%lld,\"audio_stage\":\"%s\",\"audio_fault\":%s,\"audio_session\":%s,\"audio_sent\":%llu,\"talk_played\":%llu,\"audio_channel\":%u,\"audio_rtp\":%llu,\"audio_aus\":%llu,\"audio_decode_errors\":%llu,\"talk_received\":%llu,\"talk_published\":%llu,\"audio_adts_stripped\":%llu,\"audio_last_error\":%u,\"media_broker\":%s,\"app_build\":\"0.5.3-audio-persistence-candidate\",\"motion_available\":%s,\"motion\":%s,\"motion_reason\":\"%s\"}\n",long(getpid()),unsigned(Server::GetInstance().GetFabricTable().FabricCount()),window.IsCommissioningWindowOpen()?"true":"false",self.rtc_.HasSession()?"true":"false",static_cast<unsigned long long>(sentFrames),self.video_.Enabled()?"true":"false",self.source_.Faulted()?"true":"false",self.rtc_.IsOwnerThread()?"true":"false",self.rtc_.LastStage(),radio.Fresh()?"true":"false",radio.role.c_str(),static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()),
                            self.audio_.Stage(),self.audio_.Faulted()?"true":"false",self.rtc_.AudioReady()?"true":"false",
                            static_cast<unsigned long long>(sentAudio),static_cast<unsigned long long>(self.talk_.PlayedChunks()),
                            self.audio_.Channel(),static_cast<unsigned long long>(self.audio_.RtpPackets()),static_cast<unsigned long long>(self.audio_.AccessUnits()),
                            static_cast<unsigned long long>(self.audio_.DecodeErrors()),static_cast<unsigned long long>(self.talk_.ReceivedPackets()),
                            static_cast<unsigned long long>(self.talk_.PublishedChunks()),
                            static_cast<unsigned long long>(self.audio_.AdtsStripped()),self.audio_.LastError(),std::getenv("G2H_MEDIA_BROKER_DIR")?"true":"false",motionValue?"true":"false",motionValue?(*motionValue?"true":"false"):"null",motion.reason);
                        if(!fclose(f))rename(temp,"/tmp/g2h-matter-camera-status.json");
                    }else close(fd);
                }
            }
            auto err=DeviceLayer::SystemLayer().StartTimer(System::Clock::Milliseconds32(20),Tick,&self);
            if(err==CHIP_NO_ERROR)self.timer_=true;
            else {self.video_.DisableOwnStream();self.rtc_.Shutdown();}
        }catch(...){self.video_.DisableOwnStream();self.rtc_.Shutdown();}
    }
    g2h::MotionSource motion_;
    LazyRegisteredServerCluster<g2h::MotionCluster> motionCluster_;
    bool motionRegistered_=false;
    g2h::ThreadRadio radio_;
    LazyRegisteredServerCluster<g2h::ThreadRadioCluster> radioCluster_;
    bool radioRegistered_=false;
    g2h::media::CapturePause capturePause_;
    g2h::VideoControl videoControl_;
    g2h::VideoEndpoint videoEndpoint_;
    LazyRegisteredServerCluster<g2h::ControlModeCluster> videoMode_;
    bool videoModeRegistered_=false,captureRequested_=false,videoControlInstalled_=false;
    std::optional<bool> audioPreference_;
    Clock::time_point captureReleaseAt_{};bool captureAllowed_=true;
    g2h::VideoSource source_;
    // Declared after source_ so it stops first; holds at most one JPEG.
    g2h::SnapshotWorker snapshots_{[this](std::vector<std::uint8_t>&idr,Clock::time_point&at){return source_.LatestIdr(idr,at);},
        [](const std::vector<std::uint8_t>&idr,unsigned quality,unsigned width,unsigned height){return g2h::SnapshotJpeg(idr,quality,width,height);}};
    g2h::AudioSource audio_;
    g2h::VendorControl vendor_;
    g2h::Talkback talk_{"/tmp/g2h-talkback"};
    LazyRegisteredServerCluster<g2h::ImageSettingsCluster> imageCluster_;
    LazyRegisteredServerCluster<g2h::ControlModeCluster> audioMode_,threadMode_;
    bool audioModeRegistered_=false,threadModeRegistered_=false;
    bool imageRegistered_=false,syncing_=false,sourceLost_=false;
    g2h::VideoDelegate video_;
    g2h::MatterSignaling bridge_;
    g2h::WebRtcDelegate rtc_;
    LazyRegisteredServerCluster<g2h::wp::WebRTCTransportProviderCluster> provider_;
    LazyRegisteredServerCluster<g2h::av::CameraAVStreamManagementCluster> av_;
    bool providerRegistered_=false,avRegistered_=false,timer_=false;
    Clock::time_point statusAt_{};
    bool statusSession_=false;
};
std::unique_ptr<CameraRuntime> runtime;
bool runtimeStarted=false;
void StopPreflight(chip::System::Layer*,void*){
    chip::Server::GetInstance().GenerateShutDownEvent();
    chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t){chip::DeviceLayer::PlatformMgr().StopEventLoopTask();});
}
}
void ApplicationInit(){
    runtime=std::make_unique<CameraRuntime>();
    auto err=runtime->Init();
    if(err!=CHIP_NO_ERROR){std::fprintf(stderr,"G2H_RUNTIME_INIT_ERROR %s\n",chip::ErrorStr(err));runtime.reset();chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t){
        chip::Server::GetInstance().GenerateShutDownEvent();chip::DeviceLayer::PlatformMgr().StopEventLoopTask();});return;}
    runtimeStarted=true;
    auto &window=chip::Server::GetInstance().GetCommissioningWindowManager();
    if(window.IsCommissioningWindowOpen()){window.CloseCommissioningWindow();}
    if(commissionTrial){
        if(window.OpenBasicCommissioningWindow(chip::System::Clock::Seconds32(180),chip::CommissioningWindowAdvertisement::kDnssdOnly)!=CHIP_NO_ERROR){StopPreflight(nullptr,nullptr);return;}
        windowEverOpened=true;
    }
    if(!persistentMode&&chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Seconds32(trialMode?180:8),StopPreflight,nullptr)!=CHIP_NO_ERROR)
        StopPreflight(nullptr,nullptr);
}
void ApplicationShutdown(){runtime.reset();}
int main(int argc,char **argv){
    if(argc<2){std::fputs("Use --preflight, --trial IDENTITY, --trial-commission IDENTITY or --serve.\n",stderr);return 2;}
    persistentMode=!std::strcmp(argv[1],"--serve");
    trialMode=!std::strcmp(argv[1],"--trial")||!std::strcmp(argv[1],"--trial-commission");
    commissionTrial=!std::strcmp(argv[1],"--trial-commission");
    if(!persistentMode&&!trialMode&&std::strcmp(argv[1],"--preflight"))return 2;
    const int processLock=open("/tmp/g2h-matter-camera.lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if(processLock<0||flock(processLock,LOCK_EX|LOCK_NB)){std::fputs("G2H camera app already running or lock unavailable\n",stderr);return 2;}
    g2h::Identity identity;
    if(persistentMode||trialMode){
        const char *path=persistentMode?"/mnt/config/g2h-matter-camera/identity":(argc==3?argv[2]:"");
        if((persistentMode&&argc!=2)||!identity.Load(path)){std::fputs("G2H private identity unavailable or invalid\n",stderr);return 2;}
        auto &options=LinuxDeviceOptions::GetInstance();
        options.payload.setUpPINCode=identity.pin;options.discriminator.SetValue(identity.discriminator);
        options.serialNumber.SetValue(identity.serial);options.vendorName.SetValue("G2H Community");
        options.productName.SetValue("Aqara G2H Thread & Matter Camera");
        options.hardwareVersionString.SetValue("G2H hardware 1.0");
        argc=1;argv[1]=nullptr;
    }else{
    for(int n=1;n<argc;++n)argv[n]=argv[n+1];--argc;
    }
    if(!persistentMode)alarm(trialMode?195:20);
    {std::ofstream f("/proc/self/oom_score_adj");f<<1000;if(!f)return 1;}
    rlimit vm{64*1024*1024,64*1024*1024},data{24*1024*1024,24*1024*1024};
    if(setrlimit(RLIMIT_AS,&vm)||setrlimit(RLIMIT_DATA,&data))return 1;
    if(ChipLinuxAppInit(argc,argv)!=0)return 1;
    if(persistentMode||trialMode){
        auto &store=chip::DeviceLayer::PersistedStorage::KeyValueStoreMgr();
        char bound[32];size_t length=0;auto err=store.Get("g2h-camera-identity",bound,sizeof(bound),&length);
        if(err==CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND){
            if(chip::DeviceLayer::ConfigurationMgr().StoreUniqueId(identity.unique.data(),identity.unique.size())!=CHIP_NO_ERROR||store.Put("g2h-camera-identity",identity.unique.data(),identity.unique.size())!=CHIP_NO_ERROR)return 1;
        }else if(err!=CHIP_NO_ERROR||length!=identity.unique.size()||std::memcmp(bound,identity.unique.data(),length))return 1;
    }
    LinuxDeviceOptions::GetInstance().payload.commissioningFlow=chip::CommissioningFlow::kUserActionRequired;
    std::signal(SIGUSR1,[](int){commissioningRequested=1;});
    chip::app::Clusters::InitializeTlsCertificateManagement();
    ChipLinuxAppMainLoop();
    rusage usage{};getrusage(RUSAGE_SELF,&usage);
    std::printf("G2H_PREFLIGHT {\"camera_clusters_started\":%s,\"commissioning_window_opened\":%s,\"maxrss_native\":%ld}\n",runtimeStarted?"true":"false",windowEverOpened?"true":"false",usage.ru_maxrss);
    return runtimeStarted?0:1;
}
