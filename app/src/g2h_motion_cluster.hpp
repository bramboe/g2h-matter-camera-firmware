#pragma once
#include "g2h_motion_observer.hpp"
#include <app/server-cluster/DefaultServerCluster.h>
#include <app/AttributeValueEncoder.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app/data-model/Nullable.h>
#include <lib/support/ReadOnlyBuffer.h>
#include <array>
#include <algorithm>
#include <limits>

namespace g2h {
// Development vendor cluster: observed native held-motion, not a PIR sensor or
// a standard occupancy claim. Requires a compatible controller/HA extension.
class MotionCluster final: public chip::app::DefaultServerCluster {
public:
    static constexpr chip::ClusterId kId=0xFFF1FC12;
    static constexpr chip::AttributeId kFresh=0,kHeldMotion=1,kProducerEnabled=2,kReason=3,kSource=4,kSampleAgeMs=5,kFreshnessWindowMs=6;
    explicit MotionCluster(MotionState &state):DefaultServerCluster({0,kId}),state_(state){}
    void Refresh(){
        const auto s=state_.Snapshot();const auto value=s.Value(),enabled=s.Enabled();
        const std::string reason=Reason(s);
        if(value!=lastValue_||enabled!=lastEnabled_||reason!=lastReason_){
            for(chip::AttributeId i=0;i<=3;++i)NotifyAttributeChanged(i);
            lastValue_=value;lastEnabled_=enabled;lastReason_=reason;
        }
        if(s.sampledMs!=lastSample_){NotifyAttributeChanged(kSampleAgeMs);lastSample_=s.sampledMs;}
    }
    CHIP_ERROR Attributes(const chip::app::ConcreteClusterPath &path,
        chip::ReadOnlyBufferBuilder<chip::app::DataModel::AttributeEntry> &builder)override{
        ReturnErrorOnFailure(DefaultServerCluster::Attributes(path,builder));
        using E=chip::app::DataModel::AttributeEntry;
        static const std::array<E,7> entries{{
            {0,{},chip::Access::Privilege::kView,std::nullopt},
            {1,{},chip::Access::Privilege::kView,std::nullopt},
            {2,{},chip::Access::Privilege::kView,std::nullopt},
            {3,{},chip::Access::Privilege::kView,std::nullopt},
            {4,{},chip::Access::Privilege::kView,std::nullopt},
            {5,{},chip::Access::Privilege::kView,std::nullopt},
            {6,{},chip::Access::Privilege::kView,std::nullopt}}};
        return builder.AppendElements(chip::Span(entries));
    }
    chip::app::DataModel::ActionReturnStatus ReadAttribute(const chip::app::DataModel::ReadAttributeRequest &request,
        chip::app::AttributeValueEncoder &encoder)override{
        using namespace chip;using app::DataModel::Nullable;
        const auto id=request.path.mAttributeId;const auto s=state_.Snapshot();
        if(id==app::Clusters::Globals::Attributes::FeatureMap::Id)return encoder.Encode<uint32_t>(0);
        if(id==app::Clusters::Globals::Attributes::ClusterRevision::Id)return encoder.Encode<uint16_t>(1);
        if(id==kFresh)return encoder.Encode(s.Value().has_value());
        if(id==kHeldMotion||id==kProducerEnabled){Nullable<bool> v;auto value=id==kHeldMotion?s.Value():s.Enabled();if(value)v.SetNonNull(*value);return encoder.Encode(v);}
        if(id==kReason)return encoder.Encode(CharSpan::fromCharString(Reason(s)));
        if(id==kSource)return encoder.Encode(CharSpan::fromCharString("vendor-vision-held-motion"));
        if(id==kSampleAgeMs){Nullable<uint32_t> value;auto now=motion_detail::Now();if(s.sampledMs&&now>=s.sampledMs)value.SetNonNull(uint32_t(std::min<uint64_t>(now-s.sampledMs,std::numeric_limits<uint32_t>::max())));return encoder.Encode(value);}
        if(id==kFreshnessWindowMs)return encoder.Encode<uint16_t>(2000);
        return Protocols::InteractionModel::Status::UnsupportedAttribute;
    }
private:
    static const char *Reason(const MotionObservation&s){
        auto now=motion_detail::Now();if(s.sampledMs&&(now<s.sampledMs||now-s.sampledMs>2000))return "stale";
        // All observer reasons are static constants, never a process string,
        // exception text, kernel path or captured memory dump.
        return s.reason;
    }
    MotionState &state_;std::optional<bool> lastValue_,lastEnabled_;std::string lastReason_;std::uint64_t lastSample_=0;
};
}
