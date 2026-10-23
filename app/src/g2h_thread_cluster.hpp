#pragma once
#include "g2h_thread_radio.hpp"
#include <app/server-cluster/DefaultServerCluster.h>
#include <app/AttributeValueEncoder.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <lib/support/ReadOnlyBuffer.h>
#include <array>

namespace g2h {
// Development vendor cluster, scoped to the existing test VID 0xFFF1.
// Does not masquerade as the Linux network interface or claim border routing.
// A compatible controller can read these fields under the camera's identity.
class ThreadRadioCluster final: public chip::app::DefaultServerCluster {
public:
    static constexpr chip::ClusterId kId=0xFFF1FC10;
    explicit ThreadRadioCluster(ThreadRadio &radio):DefaultServerCluster({0,kId}),radio_(radio){}
    void Refresh(){
        auto now=radio_.Snapshot();bool fresh=now.Fresh();
        if(fresh!=lastFresh_||now.role!=last_.role||now.channel!=last_.channel||now.network!=last_.network||
           now.panId!=last_.panId||now.extendedPanId!=last_.extendedPanId||now.extAddress!=last_.extAddress||now.rloc16!=last_.rloc16){
            for(chip::AttributeId id=0;id<8;++id)NotifyAttributeChanged(id);
            last_=std::move(now);lastFresh_=fresh;
        }
    }
    CHIP_ERROR Attributes(const chip::app::ConcreteClusterPath &path,
        chip::ReadOnlyBufferBuilder<chip::app::DataModel::AttributeEntry> &builder) override {
        ReturnErrorOnFailure(DefaultServerCluster::Attributes(path,builder));
        using E=chip::app::DataModel::AttributeEntry;
        static const std::array<E,9> entries{{
            {0,{},chip::Access::Privilege::kView,std::nullopt},
            {1,{},chip::Access::Privilege::kView,std::nullopt},
            {2,{},chip::Access::Privilege::kView,std::nullopt},
            {3,{},chip::Access::Privilege::kView,std::nullopt},
            {4,{},chip::Access::Privilege::kView,std::nullopt},
            {5,{},chip::Access::Privilege::kView,std::nullopt},
            {6,{},chip::Access::Privilege::kView,std::nullopt},
            {7,{},chip::Access::Privilege::kView,std::nullopt},
            {8,{},chip::Access::Privilege::kView,std::nullopt}}};
        return builder.AppendElements(chip::Span(entries));
    }
    chip::app::DataModel::ActionReturnStatus ReadAttribute(const chip::app::DataModel::ReadAttributeRequest &request,
        chip::app::AttributeValueEncoder &encoder) override {
        using namespace chip;auto s=radio_.Snapshot();auto id=request.path.mAttributeId;
        if(id==app::Clusters::Globals::Attributes::FeatureMap::Id)return encoder.Encode<uint32_t>(0);
        if(id==app::Clusters::Globals::Attributes::ClusterRevision::Id)return encoder.Encode<uint16_t>(1);
        if(id==0)return encoder.Encode(s.Fresh());
        if(id==8)return encoder.Encode(false); // This implementation is not a border router.
        if(id>8)return Protocols::InteractionModel::Status::UnsupportedAttribute;
        if(!s.Fresh())return Protocols::InteractionModel::Status::Failure;
        switch(id){
            case 1:return encoder.Encode(CharSpan::fromCharString(s.role.c_str()));
            case 2:return encoder.Encode<uint16_t>(s.channel);
            case 3:return encoder.Encode(CharSpan::fromCharString(s.network.c_str()));
            case 4:return encoder.Encode<uint16_t>(s.panId);
            case 5:return encoder.Encode(CharSpan::fromCharString(s.extendedPanId.c_str()));
            case 6:return encoder.Encode(CharSpan::fromCharString(s.extAddress.c_str()));
            case 7:return encoder.Encode<uint16_t>(s.rloc16);
        }
        return Protocols::InteractionModel::Status::UnsupportedAttribute;
    }
private:ThreadRadio &radio_;RadioSnapshot last_;bool lastFresh_=false;
};
}
