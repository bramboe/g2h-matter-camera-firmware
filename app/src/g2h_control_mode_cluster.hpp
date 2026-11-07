#pragma once
#include "g2h_language_catalog.hpp"
#include <app/server-cluster/DefaultServerCluster.h>
#include <app/AttributeValueEncoder.h>
#include <clusters/ModeSelect/Commands.h>
#include <clusters/ModeSelect/Structs.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <lib/support/ReadOnlyBuffer.h>
#include <array>
#include <functional>
#include <optional>

namespace g2h {
// Standard Mode Select transport for two explicitly described feature modes.
// Servers without our vendor model can invoke ChangeToMode. No plug/light device
// type or whole-device power claim is introduced. Pending is never reported as
// an observed enabled/disabled state.
class ControlModeCluster final:public chip::app::DefaultServerCluster {
public:
    static constexpr chip::ClusterId kId=0x0050;
    using Observe=std::function<std::optional<bool>()>;
    using Request=std::function<bool(bool)>;
    ControlModeCluster(chip::EndpointId endpoint,const char *description,Observe observe,Request request):
        DefaultServerCluster({endpoint,kId}),endpoint_(endpoint),description_(description),observe_(std::move(observe)),request_(std::move(request)){}
    const char *Description()const{
        auto translated=language::ModeDescription(language::Current(),endpoint_);
        return translated?translated:description_;
    }
    void Refresh(){
        const auto value=observe_();const auto locale=language::Current();
        if(locale!=lastLanguage_){
            lastLanguage_=locale;
            NotifyAttributeChanged(0);NotifyAttributeChanged(2);NotifyAttributeChanged(3);
        }else if(value!=last_)NotifyAttributeChanged(3);
        last_=value;
    }
    CHIP_ERROR Attributes(const chip::app::ConcreteClusterPath &path,
        chip::ReadOnlyBufferBuilder<chip::app::DataModel::AttributeEntry> &builder)override{
        ReturnErrorOnFailure(DefaultServerCluster::Attributes(path,builder));
        using E=chip::app::DataModel::AttributeEntry;
        static const std::array<E,4> entries{{
            {0,{},chip::Access::Privilege::kView,std::nullopt},
            {1,{},chip::Access::Privilege::kView,std::nullopt},
            {2,{},chip::Access::Privilege::kView,std::nullopt},
            {3,{},chip::Access::Privilege::kView,std::nullopt}}};
        return builder.AppendElements(chip::Span(entries));
    }
    chip::app::DataModel::ActionReturnStatus ReadAttribute(const chip::app::DataModel::ReadAttributeRequest &request,
        chip::app::AttributeValueEncoder &encoder)override{
        using namespace chip;const auto id=request.path.mAttributeId;
        if(id==app::Clusters::Globals::Attributes::FeatureMap::Id)return encoder.Encode<uint32_t>(0);
        if(id==app::Clusters::Globals::Attributes::ClusterRevision::Id)return encoder.Encode<uint16_t>(2);
        if(id==0)return encoder.Encode(CharSpan::fromCharString(Description()));
        if(id==1){app::DataModel::Nullable<uint16_t> value;return encoder.Encode(value);}
        if(id==2){
            using Mode=app::Clusters::ModeSelect::Structs::ModeOptionStruct::Type;
            std::array<Mode,2> modes{};
            const auto locale=language::Current();
            modes[0].label=CharSpan::fromCharString(language::ControlOption(locale,false));modes[0].mode=0;
            modes[1].label=CharSpan::fromCharString(language::ControlOption(locale,true));modes[1].mode=1;
            return encoder.Encode(app::DataModel::List<const Mode>(modes.data(),modes.size()));
        }
        if(id==3){const auto value=observe_();if(value)return encoder.Encode<uint8_t>(*value?1:0);return Protocols::InteractionModel::Status::Failure;}
        return Protocols::InteractionModel::Status::UnsupportedAttribute;
    }
    CHIP_ERROR AcceptedCommands(const chip::app::ConcreteClusterPath &,
        chip::ReadOnlyBufferBuilder<chip::app::DataModel::AcceptedCommandEntry> &builder)override{
        using E=chip::app::DataModel::AcceptedCommandEntry;
        static const std::array<E,1> entries{{{0,{},chip::Access::Privilege::kOperate}}};
        return builder.AppendElements(chip::Span(entries));
    }
    std::optional<chip::app::DataModel::ActionReturnStatus> InvokeCommand(
        const chip::app::DataModel::InvokeRequest &request,chip::TLV::TLVReader &arguments,chip::app::CommandHandler *)override{
        using namespace chip;
        if(request.path.mCommandId!=0)return Protocols::InteractionModel::Status::UnsupportedCommand;
        app::Clusters::ModeSelect::Commands::ChangeToMode::DecodableType data;
        ReturnErrorOnFailure(data.Decode(arguments));
        if(data.newMode>1)return Protocols::InteractionModel::Status::InvalidCommand;
        if(!request_(data.newMode!=0))return Protocols::InteractionModel::Status::Failure;
        NotifyAttributeChanged(3);
        return Protocols::InteractionModel::Status::Success; // Queued; read actual state.
    }
private:
    chip::EndpointId endpoint_;const char *description_;Observe observe_;Request request_;std::optional<bool> last_;
    language::Id lastLanguage_=language::Current();
};
}
