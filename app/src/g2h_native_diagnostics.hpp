#pragma once
#include "g2h_thread_radio.hpp"
#include <app/server-cluster/DefaultServerCluster.h>
#include <app/AttributeValueEncoder.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <lib/support/ReadOnlyBuffer.h>
#include <app/data-model/Nullable.h>
#include <array>
#include <functional>
#include <optional>
#include <tuple>

namespace g2h {
// Read-only controller compatibility projection of the separate JN5189. This
// does not register the Linux Wi-Fi interface as Thread or enable commissioning.
// Only actually observed attributes are exposed; no fabricated topology/counters.
class NativeThreadDiagnostics final:public chip::app::DefaultServerCluster {
public:
    static constexpr chip::ClusterId kId=0x0035;
    using Observe=std::function<RadioSnapshot()>;
    explicit NativeThreadDiagnostics(ThreadRadio &radio):NativeThreadDiagnostics([&radio]{return radio.Snapshot();}){}
    explicit NativeThreadDiagnostics(Observe observe):DefaultServerCluster({2,kId}),observe_(std::move(observe)){}
    static std::optional<uint8_t> RoutingRole(const std::string &role){
        if(role=="disabled")return 0; // Unspecified, not an attached router.
        if(role=="detached")return 1;
        if(role=="child")return 4; // This firmware is a router-eligible FTD.
        if(role=="router")return 5;
        if(role=="leader")return 6;
        return {};
    }
    void Refresh(){
        const auto s=observe_();
        auto now=std::make_tuple(s.Fresh(),s.role,s.channel,s.network);
        if(now!=last_){for(chip::AttributeId id=0;id<3;++id)NotifyAttributeChanged(id);last_=std::move(now);}
    }
    CHIP_ERROR Attributes(const chip::app::ConcreteClusterPath &path,
        chip::ReadOnlyBufferBuilder<chip::app::DataModel::AttributeEntry> &builder)override{
        ReturnErrorOnFailure(DefaultServerCluster::Attributes(path,builder));
        using E=chip::app::DataModel::AttributeEntry;
        static const std::array<E,3> entries{{
            {0,{},chip::Access::Privilege::kView,std::nullopt},
            {1,{},chip::Access::Privilege::kView,std::nullopt},
            {2,{},chip::Access::Privilege::kView,std::nullopt}}};
        return builder.AppendElements(chip::Span(entries));
    }
    chip::app::DataModel::ActionReturnStatus ReadAttribute(const chip::app::DataModel::ReadAttributeRequest &request,
        chip::app::AttributeValueEncoder &encoder)override{
        using namespace chip;const auto id=request.path.mAttributeId;const auto s=observe_();
        if(id==app::Clusters::Globals::Attributes::FeatureMap::Id)return encoder.Encode<uint32_t>(0);
        if(id==app::Clusters::Globals::Attributes::ClusterRevision::Id)return encoder.Encode<uint16_t>(3);
        if(id==0){app::DataModel::Nullable<uint16_t> value;if(s.Fresh()&&s.channel>=11&&s.channel<=26)value.SetNonNull(uint16_t(s.channel));return encoder.Encode(value);}
        if(id==1){app::DataModel::Nullable<uint8_t> value;const auto role=RoutingRole(s.role);if(s.Fresh()&&role)value.SetNonNull(*role);return encoder.Encode(value);}
        if(id==2){app::DataModel::Nullable<CharSpan> value;if(s.Fresh()&&!s.network.empty()&&s.network.size()<=16)value.SetNonNull(CharSpan::fromCharString(s.network.c_str()));return encoder.Encode(value);}
        return Protocols::InteractionModel::Status::UnsupportedAttribute;
    }
private:
    Observe observe_;std::tuple<bool,std::string,unsigned,std::string> last_{};
};

// Camera-derived motion, carried by standard vision Occupancy Sensing. It is a
// held motion indication, not person recognition or continuous presence proof.
class NativeVisionMotion final:public chip::app::DefaultServerCluster {
public:
    static constexpr chip::ClusterId kId=0x0406;
    using Observe=std::function<std::optional<bool>()>;
    explicit NativeVisionMotion(Observe observe):DefaultServerCluster({1,kId}),observe_(std::move(observe)){}
    void Refresh(){auto value=observe_();if(value!=last_){last_=value;NotifyAttributeChanged(0);}}
    CHIP_ERROR Attributes(const chip::app::ConcreteClusterPath &path,
        chip::ReadOnlyBufferBuilder<chip::app::DataModel::AttributeEntry> &builder)override{
        ReturnErrorOnFailure(DefaultServerCluster::Attributes(path,builder));
        using E=chip::app::DataModel::AttributeEntry;
        static const std::array<E,2> entries{{
            {0,{},chip::Access::Privilege::kView,std::nullopt},
            {2,{},chip::Access::Privilege::kView,std::nullopt}}};
        return builder.AppendElements(chip::Span(entries));
    }
    chip::app::DataModel::ActionReturnStatus ReadAttribute(const chip::app::DataModel::ReadAttributeRequest &request,
        chip::app::AttributeValueEncoder &encoder)override{
        using namespace chip;const auto id=request.path.mAttributeId;
        if(id==app::Clusters::Globals::Attributes::FeatureMap::Id)return encoder.Encode<uint32_t>(0x80); // Vision only.
        if(id==app::Clusters::Globals::Attributes::ClusterRevision::Id)return encoder.Encode<uint16_t>(5);
        if(id==0){auto value=observe_();if(!value)return Protocols::InteractionModel::Status::Failure;return encoder.Encode<uint8_t>(*value?1:0);}
        // Legacy physical modalities do not describe a camera. Do not claim a
        // PIR/ultrasonic/contact sensor or transmit the unknown-enum sentinel.
        if(id==2)return encoder.Encode<uint8_t>(0);
        return Protocols::InteractionModel::Status::UnsupportedAttribute;
    }
private:
    Observe observe_;std::optional<bool> last_;
};
}
