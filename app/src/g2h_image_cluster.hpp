#pragma once
#include "g2h_vendor_control.hpp"
#include <app/server-cluster/DefaultServerCluster.h>
#include <app/AttributeValueDecoder.h>
#include <app/AttributeValueEncoder.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <lib/support/ReadOnlyBuffer.h>
#include <array>
#include <functional>
#include <app/data-model/Nullable.h>

namespace g2h {
// Development vendor cluster (test VID 0xFFF1) for camera-wide settings without a
// fitting Matter attribute: image brightness and speaker volume (0..100) and the
// timestamp overlay (bool). Matter's OSDEnabled is per stream and HA's server
// sends false by default, which would switch the overlay off for RTSP/HomeKit too.
// Standard controllers do not render this cluster.
class ImageSettingsCluster final: public chip::app::DefaultServerCluster {
public:
    static constexpr chip::ClusterId kId=0xFFF1FC11;
    static constexpr chip::AttributeId kBrightness=0,kSpeakerVolume=1,kOsd=2,kAudioEnabled=3;
    explicit ImageSettingsCluster(VendorControl &vendor,std::function<void()> beforeAudioChange={},
        std::function<bool(bool)> requestAudioChange={}):
        DefaultServerCluster({1,kId}),vendor_(vendor),beforeAudioChange_(std::move(beforeAudioChange)),requestAudioChange_(std::move(requestAudioChange)){}
    void Changed(Vendor which){
        if(which==Vendor::Brightness)NotifyAttributeChanged(kBrightness);
        if(which==Vendor::SpeakerVolume)NotifyAttributeChanged(kSpeakerVolume);
        if(which==Vendor::Osd)NotifyAttributeChanged(kOsd);
        if(IsAudioMute(which))NotifyAttributeChanged(kAudioEnabled);
    }
    CHIP_ERROR Attributes(const chip::app::ConcreteClusterPath &path,
        chip::ReadOnlyBufferBuilder<chip::app::DataModel::AttributeEntry> &builder) override {
        ReturnErrorOnFailure(DefaultServerCluster::Attributes(path,builder));
        using E=chip::app::DataModel::AttributeEntry;
        static const std::array<E,4> entries{{
            {kBrightness,{},chip::Access::Privilege::kView,chip::Access::Privilege::kOperate},
            {kSpeakerVolume,{},chip::Access::Privilege::kView,chip::Access::Privilege::kOperate},
            {kOsd,{},chip::Access::Privilege::kView,chip::Access::Privilege::kOperate},
            {kAudioEnabled,{},chip::Access::Privilege::kView,chip::Access::Privilege::kOperate}}};
        return builder.AppendElements(chip::Span(entries));
    }
    chip::app::DataModel::ActionReturnStatus ReadAttribute(const chip::app::DataModel::ReadAttributeRequest &request,
        chip::app::AttributeValueEncoder &encoder) override {
        using namespace chip;const auto id=request.path.mAttributeId;
        if(id==app::Clusters::Globals::Attributes::FeatureMap::Id)return encoder.Encode<uint32_t>(0);
        if(id==app::Clusters::Globals::Attributes::ClusterRevision::Id)return encoder.Encode<uint16_t>(2);
        if(id==kAudioEnabled){
            app::DataModel::Nullable<bool> value;const auto muted=vendor_.AudioMuted();
            if(muted)value.SetNonNull(!*muted);
            return encoder.Encode(value);
        }
        const auto which=Map(id);if(!which)return Protocols::InteractionModel::Status::UnsupportedAttribute;
        const auto value=vendor_.Value(*which);if(!value)return Protocols::InteractionModel::Status::Failure;
        if(id==kOsd)return encoder.Encode(*value!=0);
        return encoder.Encode<uint8_t>(uint8_t(*value));
    }
    chip::app::DataModel::ActionReturnStatus WriteAttribute(const chip::app::DataModel::WriteAttributeRequest &request,
        chip::app::AttributeValueDecoder &decoder) override {
        using namespace chip;const auto id=request.path.mAttributeId;
        if(id==kAudioEnabled){
            bool enabled=false;ReturnErrorOnFailure(decoder.Decode(enabled));
            if(requestAudioChange_){
                if(!requestAudioChange_(enabled))return Protocols::InteractionModel::Status::Failure;
            }else{
                if(beforeAudioChange_)beforeAudioChange_();
                if(!vendor_.WriteAudioMuted(!enabled))return Protocols::InteractionModel::Status::Failure;
            }
            NotifyAttributeChanged(id);return Protocols::InteractionModel::Status::Success;
        }
        const auto which=Map(id);if(!which)return Protocols::InteractionModel::Status::UnsupportedWrite;
        uint8_t value=0;
        if(id==kOsd){bool on=false;ReturnErrorOnFailure(decoder.Decode(on));value=on;}
        else ReturnErrorOnFailure(decoder.Decode(value));
        if(value>100)return Protocols::InteractionModel::Status::ConstraintError;
        if(!vendor_.Write(*which,value))return Protocols::InteractionModel::Status::Failure;
        NotifyAttributeChanged(id);return Protocols::InteractionModel::Status::Success;
    }
private:
    static std::optional<Vendor> Map(chip::AttributeId id){
        if(id==kBrightness)return Vendor::Brightness;
        if(id==kSpeakerVolume)return Vendor::SpeakerVolume;
        if(id==kOsd)return Vendor::Osd;
        return std::nullopt;
    }
    VendorControl &vendor_;
    std::function<void()> beforeAudioChange_;
    std::function<bool(bool)> requestAudioChange_;
};
}
