#pragma once
#include <app/clusters/camera-av-stream-management-server/CameraAVStreamManagementCluster.h>
#include <functional>
#include <limits>

namespace g2h {
namespace av = chip::app::Clusters::CameraAvStreamManagement;
using Status = chip::Protocols::InteractionModel::Status;

// One read-only hardware encoder, reused through RTSP. These delegates never
// reconfigure the vendor encoder or promise bitrate/GOP control we do not have.
class VideoDelegate final : public av::CameraAVStreamManagementDelegate {
public:
    struct Format { std::uint16_t width, height, fps; };
    using Start = std::function<bool()>;
    using Stop = std::function<void()>;
    using Capture = std::function<Status(unsigned,av::ImageSnapshot&)>;
    // Optional, non-blocking: start preparing a snapshot at this quality.
    using Prepare = std::function<void(unsigned)>;
    // Optional: start (idempotently) the audio ingest at this Opus bit rate.
    using AudioStart = std::function<bool(unsigned)>;
    static constexpr std::uint16_t kAudioStreamId=3;
    // Optional hooks for camera controls; called on the Matter thread.
    std::function<void(chip::AttributeId)> attributeChanged;
    VideoDelegate(Format format, Start start, Stop stop, Capture capture={}, Prepare prepare={}, AudioStart audio={})
        : format_(format), start_(std::move(start)), stop_(std::move(stop)),capture_(std::move(capture)),prepare_(std::move(prepare)),audioStart_(std::move(audio)) {}
    ~VideoDelegate() override { StopOwnStream(); }
    VideoDelegate(const VideoDelegate&)=delete;
    VideoDelegate& operator=(const VideoDelegate&)=delete;

    Status VideoStreamAllocate(const av::VideoStreamStruct &r, std::uint16_t &id) override {
        if(!enabled_ || fault_) return Status::InvalidInState;
        if(!Compatible(r)) return Status::DynamicConstraintError;
        if(reserved_) return Status::ResourceExhausted;
        if(!streams_.empty()&&active_){id=1;return Status::Success;}
        bool ready=false;
        try { ready=start_ && start_(); } catch(...) { ready=false; }
        if(!ready) {
            try { if(stop_) stop_(); } catch(...) {}
            return Status::Failure; // transient (e.g. RTSP not ready yet); a later request retries
        }
        prepared_=true; reserved_=true; id=1; return Status::Success;
    }
    void OnVideoStreamAllocated(const av::VideoStreamStruct &r, av::StreamAllocationAction action) override {
        if(action==av::StreamAllocationAction::kReuse && active_ && r.videoStreamID==1) return;
        if(!reserved_ || r.videoStreamID!=1 || !Compatible(r)) { fault_=true; StopOwnStream(); return; }
        // Preparation opened the source; frames may be released only after the
        // SDK supplies the final compatible allocation here.
        if(!prepared_) { fault_=true; return; }
        active_=true; streams_={r}; reserved_=false;
    }
    Status VideoStreamModify(std::uint16_t id, chip::Optional<bool> watermark, chip::Optional<bool> osd) override {
        if(!Allocated(id)) return Status::NotFound;
        if(watermark.HasValue()) return Status::UnsupportedCommand;
        // The vendor OSD is global (also RTSP/HomeKit); it is a G2H vendor attribute, not per stream.
        (void)osd;
        return Status::Success;
    }
    Status VideoStreamDeallocate(std::uint16_t id) override {
        if(!Allocated(id)) return Status::NotFound;
        if(streams_[0].referenceCount) return Status::InvalidInState;
        StopOwnStream(); streams_.clear(); reserved_=false; return Status::Success;
    }
    // One Opus mono 48 kHz stream transcoded from the camera's AAC track.
    Status AudioStreamAllocate(const av::AudioStreamStruct &r,std::uint16_t &id) override {
        if(!audioStart_)return Status::UnsupportedCommand;
        if(!Enabled())return Status::InvalidInState;
        if(r.streamUsage!=av::StreamUsageEnum::kLiveView||r.audioCodec!=av::AudioCodecEnum::kOpus||r.channelCount!=1||
           r.sampleRate!=48000||r.bitDepth!=16||r.bitRate<6000||r.bitRate>128000)return Status::DynamicConstraintError;
        if(AudioAllocated(kAudioStreamId)){id=kAudioStreamId;return Status::Success;}
        bool ready=false;try{ready=audioStart_(r.bitRate);}catch(...){}
        if(!ready)return Status::Failure;
        audioBitRate_=r.bitRate;audioStreams_={r};audioStreams_[0].audioStreamID=kAudioStreamId;id=kAudioStreamId;return Status::Success;
    }
    Status AudioStreamDeallocate(std::uint16_t id) override {
        if(!AudioAllocated(id))return audioStart_?Status::NotFound:Status::UnsupportedCommand;
        for(const auto &stream:GetAllocatedAudioStreams())if(stream.audioStreamID==id&&stream.referenceCount)return Status::InvalidInState;
        audioStreams_.clear();return Status::Success; // The ingest stays open; frames stop with the session.
    }
    Status SnapshotStreamAllocate(const SnapshotStreamAllocateArgs &r,std::uint16_t &id) override {
        if(!capture_)return Status::UnsupportedCommand;
        if(!Enabled())return Status::InvalidInState;
        if(r.imageCodec!=av::ImageCodecEnum::kJpeg||r.maxFrameRate!=1||!r.quality||r.quality>100||
           !r.minResolution.width||!r.minResolution.height||r.minResolution.width>320||r.minResolution.height>180||
           r.maxResolution.width<320||r.maxResolution.height<180||r.encodedPixels||
           r.watermarkEnabled.HasValue())return Status::DynamicConstraintError; // OSD comes from the shared encoder
        bool ready=false;try{ready=start_&&start_();}catch(...){}
        if(!ready)return Status::Failure;
        snapshotQuality_=r.quality;snapshotAllocated_=true;id=2;
        if(prepare_){try{prepare_(r.quality);}catch(...){}}
        return Status::Success;
    }
    Status SnapshotStreamModify(std::uint16_t id,chip::Optional<bool> watermark,chip::Optional<bool> osd) override {
        if(!SnapshotAllocated(id))return capture_?Status::NotFound:Status::UnsupportedCommand;
        (void)osd;return watermark.HasValue()?Status::UnsupportedCommand:Status::Success;
    }
    Status SnapshotStreamDeallocate(std::uint16_t id) override {
        if(!SnapshotAllocated(id))return capture_?Status::NotFound:Status::UnsupportedCommand;
        snapshotAllocated_=false;return Status::Success;
    }
    Status CaptureSnapshot(chip::app::DataModel::Nullable<std::uint16_t> id,const av::VideoResolutionStruct &resolution,av::ImageSnapshot &image) override {
        if(!capture_)return Status::UnsupportedCommand;
        if(!Enabled())return Status::InvalidInState;
        if(!SnapshotAllocated(id.IsNull()?2:id.Value()))return Status::NotFound;
        // RequestedResolution is a preference; the response states the real 320x180.
        (void)resolution;
        unsigned quality=snapshotQuality_;
        if(auto *cluster=GetCameraAVStreamManagementCluster()){
            for(const auto &stream:cluster->GetAllocatedSnapshotStreams())if(stream.snapshotStreamID==2)quality=stream.quality;
        }
        try{return capture_(quality,image);}catch(...){return Status::Failure;}
    }
    void OnStreamUsagePrioritiesChanged() override {}
    void OnAttributeChanged(chip::AttributeId id) override {if(attributeChanged)attributeChanged(id);}
    CHIP_ERROR PersistentAttributesLoadedCallback() override {
        auto *cluster=GetCameraAVStreamManagementCluster();if(!cluster)return CHIP_NO_ERROR;
        for(const auto &audio:cluster->GetAllocatedAudioStreams()){
            if(audio.audioStreamID!=kAudioStreamId)continue;
            audioBitRate_=audio.bitRate;
            while(true){
                unsigned count=0;for(const auto &a:cluster->GetAllocatedAudioStreams())if(a.audioStreamID==kAudioStreamId)count=a.referenceCount;
                if(!count)break;
                auto err=cluster->UpdateAudioStreamRefCount(kAudioStreamId,false);if(err!=CHIP_NO_ERROR){fault_=true;return err;}
            }
            break;
        }
        const auto &loaded=cluster->GetAllocatedVideoStreams();
        if(loaded.empty()){streams_.clear();return CHIP_NO_ERROR;}
        if(loaded.size()!=1||loaded[0].videoStreamID!=1||!Compatible(loaded[0])){fault_=true;return CHIP_ERROR_INVALID_ARGUMENT;}
        // Secure sessions do not survive restart. Persisted reference counts
        // must not pin an allocation to a session which no longer exists.
        while(cluster->GetAllocatedVideoStreams()[0].referenceCount){
            auto err=cluster->UpdateVideoStreamRefCount(1,false);if(err!=CHIP_NO_ERROR){fault_=true;return err;}
        }
        streams_=loaded;active_=false;prepared_=false;reserved_=false;
        return CHIP_NO_ERROR;
    }
    const std::vector<av::VideoStreamStruct>& GetAllocatedVideoStreams() const override {
        auto *cluster=GetCameraAVStreamManagementCluster();
        return cluster ? cluster->GetAllocatedVideoStreams() : streams_;
    }
    const std::vector<av::AudioStreamStruct>& GetAllocatedAudioStreams() const override {
        auto *cluster=GetCameraAVStreamManagementCluster();
        return cluster ? cluster->GetAllocatedAudioStreams() : audioStreams_;
    }
    bool AudioAllocated(std::uint16_t id) const {
        if(!audioStart_||id!=kAudioStreamId)return false;
        for(const auto &stream:GetAllocatedAudioStreams())if(stream.audioStreamID==id)return true;
        return false;
    }
    // A session using audio (re)starts the ingest, also for a stream restored from storage.
    CHIP_ERROR AcquireAudioReference() {
        if(!AudioAllocated(kAudioStreamId))return CHIP_ERROR_INCORRECT_STATE;
        bool ready=false;try{ready=audioStart_(audioBitRate_);}catch(...){}
        if(!ready)return CHIP_ERROR_INTERNAL;
        if(auto *cluster=GetCameraAVStreamManagementCluster())return cluster->UpdateAudioStreamRefCount(kAudioStreamId,true);
        ++audioStreams_[0].referenceCount;return CHIP_NO_ERROR;
    }
    CHIP_ERROR ReleaseAudioReference() {
        if(!AudioAllocated(kAudioStreamId))return CHIP_ERROR_INCORRECT_STATE;
        if(auto *cluster=GetCameraAVStreamManagementCluster())return cluster->UpdateAudioStreamRefCount(kAudioStreamId,false);
        if(audioStreams_[0].referenceCount)--audioStreams_[0].referenceCount;return CHIP_NO_ERROR;
    }
    bool Active() const {return active_ && enabled_ && !fault_;}
    bool Faulted() const {return fault_;}
    bool Allocated(std::uint16_t id) const {return id==1 && streams_.size()==1;}
    bool Enabled() const {return enabled_&&!fault_&&!privacy_;}
    // App-level livestream privacy: this app stops sending video/audio over Matter,
    // but the vendor video_on, RTSP and HomeKit are untouched. Reversible.
    void SetLivestreamPrivacy(bool on){if(privacy_==on)return;privacy_=on;if(on)StopOwnStream();}
    bool LivestreamPrivacy() const {return privacy_;}
    // Keep the allocation, but release only this app's RTSP connection when
    // its last WebRTC session ends. A later session may reopen the source.
    void SuspendOwnSource() {StopOwnStream();}
    // The ingest failed: keep the allocation and stay enabled, but the next session must reopen it.
    void SourceLost() noexcept {active_=false;prepared_=false;}
    CHIP_ERROR ResumeOwnSource() {
        if(!Enabled()||!Allocated(1))return CHIP_ERROR_INCORRECT_STATE;
        if(active_)return CHIP_NO_ERROR;
        bool ready=false;try {ready=start_&&start_();}catch(...){}
        if(!ready){try{if(stop_)stop_();}catch(...){}return CHIP_ERROR_INTERNAL;} // transient; retried by the next session
        prepared_=true;active_=true;return CHIP_NO_ERROR;
    }
    CHIP_ERROR AcquireReference() {
        if(!Allocated(1)||streams_[0].referenceCount)return CHIP_ERROR_INCORRECT_STATE;
        auto *cluster=GetCameraAVStreamManagementCluster();
        if(cluster){auto err=cluster->UpdateVideoStreamRefCount(1,true);if(err!=CHIP_NO_ERROR)return err;}
        ++streams_[0].referenceCount;return CHIP_NO_ERROR;
    }
    CHIP_ERROR ReleaseReference() {
        if(!Allocated(1)||!streams_[0].referenceCount)return CHIP_ERROR_INCORRECT_STATE;
        auto *cluster=GetCameraAVStreamManagementCluster();
        if(cluster){auto err=cluster->UpdateVideoStreamRefCount(1,false);if(err!=CHIP_NO_ERROR)return err;}
        --streams_[0].referenceCount;return CHIP_NO_ERROR;
    }
    // This only disables this application's own stream, never global privacy.
    void DisableOwnStream() {enabled_=false; StopOwnStream();}
private:
    bool SnapshotAllocated(std::uint16_t id) const {
        if(!capture_||id!=2)return false;
        if(auto *cluster=GetCameraAVStreamManagementCluster()){
            for(const auto &stream:cluster->GetAllocatedSnapshotStreams())if(stream.snapshotStreamID==2&&stream.imageCodec==av::ImageCodecEnum::kJpeg)return true;
            return false;
        }
        return snapshotAllocated_;
    }
    bool Compatible(const av::VideoStreamStruct &r) const {
        return format_.width && format_.height && format_.fps &&
            r.streamUsage==av::StreamUsageEnum::kLiveView && r.videoCodec==av::VideoCodecEnum::kH264 &&
            r.minResolution.width>=1 && r.minResolution.height>=1 &&
            r.minResolution.width<=format_.width && r.maxResolution.width>=format_.width &&
            r.minResolution.height<=format_.height && r.maxResolution.height>=format_.height &&
            r.minFrameRate>=1 && r.minFrameRate<=format_.fps && r.maxFrameRate>=format_.fps &&
            // The vendor encoder's bit rate and GOP are fixed; any valid range is
            // accepted (HA's Matter server asks 1..8 Mbit/s with a 4000 ms key frame interval).
            r.minBitRate>=1 && r.minBitRate<=r.maxBitRate &&
            !r.watermarkEnabled.HasValue();
    }
    void StopOwnStream() noexcept {active_=false;if(prepared_){prepared_=false;try{if(stop_)stop_();}catch(...){fault_=true;}}}
    Format format_;
    Start start_;
    Stop stop_;
    Capture capture_;
    Prepare prepare_;
    AudioStart audioStart_;
    unsigned audioBitRate_=32000;
    std::vector<av::AudioStreamStruct> audioStreams_;
    bool snapshotAllocated_=false;
    unsigned snapshotQuality_=70;
    bool reserved_=false, prepared_=false, active_=false, enabled_=true, fault_=false, privacy_=false;
    std::vector<av::VideoStreamStruct> streams_;
};
}
