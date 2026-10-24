#pragma once
#include "g2h_thread_radio.hpp"
#include <app/server-cluster/DefaultServerCluster.h>
#include <app/AttributeValueEncoder.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <lib/support/ReadOnlyBuffer.h>
#include <array>
#include <app/data-model/Nullable.h>

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
        unsigned mask=now.neighbors.Fresh()|(now.children.Fresh()<<1)|(now.routers.Fresh()<<2)|(now.counters.Fresh()<<3);
        // Dataset timestamps change only on complete parses. Notify subscribers
        // independently when optional telemetry expires or a new sample arrives.
        if(now.sampled!=last_.sampled||fresh!=lastFresh_||
           mask!=lastMask_){
            for(chip::AttributeId id=0;id<=23;++id)NotifyAttributeChanged(id);
            last_=std::move(now);lastFresh_=fresh;lastMask_=mask;
        }
    }
    CHIP_ERROR Attributes(const chip::app::ConcreteClusterPath &path,
        chip::ReadOnlyBufferBuilder<chip::app::DataModel::AttributeEntry> &builder) override {
        ReturnErrorOnFailure(DefaultServerCluster::Attributes(path,builder));
        using E=chip::app::DataModel::AttributeEntry;
        static const std::array<E,24> entries{{
            {0,{},chip::Access::Privilege::kView,std::nullopt},
            {1,{},chip::Access::Privilege::kView,std::nullopt},
            {2,{},chip::Access::Privilege::kView,std::nullopt},
            {3,{},chip::Access::Privilege::kView,std::nullopt},
            {4,{},chip::Access::Privilege::kView,std::nullopt},
            {5,{},chip::Access::Privilege::kView,std::nullopt},
            {6,{},chip::Access::Privilege::kView,std::nullopt},
            {7,{},chip::Access::Privilege::kView,std::nullopt},
            {8,{},chip::Access::Privilege::kView,std::nullopt},
            {9,{},chip::Access::Privilege::kView,std::nullopt},
            {10,{},chip::Access::Privilege::kView,std::nullopt},
            {11,{},chip::Access::Privilege::kView,std::nullopt},
            {12,{},chip::Access::Privilege::kView,std::nullopt},
            {13,{},chip::Access::Privilege::kView,std::nullopt},
            {14,{},chip::Access::Privilege::kView,std::nullopt},
            {15,{},chip::Access::Privilege::kView,std::nullopt},
            {16,{},chip::Access::Privilege::kView,std::nullopt},
            {17,{},chip::Access::Privilege::kView,std::nullopt},
            {18,{},chip::Access::Privilege::kView,std::nullopt},
            {19,{},chip::Access::Privilege::kView,std::nullopt},
            {20,{},chip::Access::Privilege::kView,std::nullopt},
            {21,{},chip::Access::Privilege::kView,std::nullopt},
            {22,{},chip::Access::Privilege::kView,std::nullopt},
            {23,{},chip::Access::Privilege::kView,std::nullopt}}};
        return builder.AppendElements(chip::Span(entries));
    }
    chip::app::DataModel::ActionReturnStatus ReadAttribute(const chip::app::DataModel::ReadAttributeRequest &request,
        chip::app::AttributeValueEncoder &encoder) override {
        using namespace chip;auto s=radio_.Snapshot();auto id=request.path.mAttributeId;
        if(id==app::Clusters::Globals::Attributes::FeatureMap::Id)return encoder.Encode<uint32_t>(0);
        if(id==app::Clusters::Globals::Attributes::ClusterRevision::Id)return encoder.Encode<uint16_t>(2);
        if(id==0)return encoder.Encode(s.Fresh());
        if(id==8)return encoder.Encode(false); // This implementation is not a border router.
        if(id>=9&&id<=23){
            using chip::app::DataModel::Nullable;
            const RadioDataset *d=id<=11?&s.neighbors:id<=14?&s.children:id<=17?&s.routers:nullptr;
            if(d){unsigned field=(id-9)%3;
                if(field==0)return encoder.Encode(d->Fresh());
                if(field==1){Nullable<uint16_t> v;if(d->Fresh())v.SetNonNull(d->count);return encoder.Encode(v);}
                std::string preview;Nullable<CharSpan> v;if(d->Fresh()){preview=RadioTablePreview(d->table);v.SetNonNull(CharSpan::fromCharString(preview.c_str()));}return encoder.Encode(v);
            }
            if(id==18)return encoder.Encode(s.counters.Fresh());
            if(id==22){Nullable<uint64_t> v;if(s.counters.Fresh())v.SetNonNull(s.counters.uptimeMs);return encoder.Encode(v);}
            if(id==23)return encoder.Encode<uint16_t>(90); // Freshness window, seconds.
            Nullable<uint64_t> v;if(s.counters.Fresh())v.SetNonNull(id==19?s.counters.tx:id==20?s.counters.rx:s.counters.retry);return encoder.Encode(v);
        }
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
private:ThreadRadio &radio_;RadioSnapshot last_;bool lastFresh_=false;unsigned lastMask_=0;
};
}
