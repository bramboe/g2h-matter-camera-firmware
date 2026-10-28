#pragma once
#include "g2h_language_service.hpp"
#include "g2h_control_mode_cluster.hpp"

namespace g2h {
class LanguageModeCluster final:public chip::app::DefaultServerCluster {
public:
    explicit LanguageModeCluster(LanguageService &service):DefaultServerCluster({7,0x50}),service_(service){}
    void Refresh(){auto id=service_.Snapshot().observed;if(id!=last_){last_=id;NotifyAttributeChanged(3);}}
    CHIP_ERROR Attributes(const chip::app::ConcreteClusterPath &p,chip::ReadOnlyBufferBuilder<chip::app::DataModel::AttributeEntry> &b)override{
        ReturnErrorOnFailure(DefaultServerCluster::Attributes(p,b));using E=chip::app::DataModel::AttributeEntry;
        static const std::array<E,4> e{{{0,{},chip::Access::Privilege::kView,std::nullopt},{1,{},chip::Access::Privilege::kView,std::nullopt},{2,{},chip::Access::Privilege::kView,std::nullopt},{3,{},chip::Access::Privilege::kView,std::nullopt}}};return b.AppendElements(chip::Span(e));}
    chip::app::DataModel::ActionReturnStatus ReadAttribute(const chip::app::DataModel::ReadAttributeRequest &r,chip::app::AttributeValueEncoder &e)override{
        using namespace chip;const auto id=r.path.mAttributeId;
        if(id==0xfffc)return e.Encode<uint32_t>(0);if(id==0xfffd)return e.Encode<uint16_t>(2);
        if(id==0)return e.Encode(CharSpan::fromCharString(language::kSelectorDescription));
        if(id==1){app::DataModel::Nullable<uint16_t> v;return e.Encode(v);}
        if(id==2){using M=app::Clusters::ModeSelect::Structs::ModeOptionStruct::Type;std::array<M,2> modes{};
            for(unsigned n=0;n<modes.size();++n){modes[n].mode=n;modes[n].label=CharSpan::fromCharString(language::Name(*language::FromMode(n)));}
            return e.Encode(app::DataModel::List<const M>(modes.data(),modes.size()));}
        if(id==3){auto v=service_.Snapshot().observed;if(v)return e.Encode<uint8_t>(static_cast<uint8_t>(*v));return Protocols::InteractionModel::Status::Failure;}
        return Protocols::InteractionModel::Status::UnsupportedAttribute;
    }
    CHIP_ERROR AcceptedCommands(const chip::app::ConcreteClusterPath &,chip::ReadOnlyBufferBuilder<chip::app::DataModel::AcceptedCommandEntry> &b)override{
        using E=chip::app::DataModel::AcceptedCommandEntry;static const std::array<E,1> e{{{0,{},chip::Access::Privilege::kManage}}};return b.AppendElements(chip::Span(e));}
    std::optional<chip::app::DataModel::ActionReturnStatus> InvokeCommand(const chip::app::DataModel::InvokeRequest &r,chip::TLV::TLVReader &a,chip::app::CommandHandler *)override{
        using namespace chip;if(r.path.mCommandId!=0)return Protocols::InteractionModel::Status::UnsupportedCommand;
        app::Clusters::ModeSelect::Commands::ChangeToMode::DecodableType d;ReturnErrorOnFailure(d.Decode(a));const auto id=language::FromMode(d.newMode);
        if(!id)return Protocols::InteractionModel::Status::InvalidCommand;
        return service_.Request(*id)?Protocols::InteractionModel::Status::Success:Protocols::InteractionModel::Status::Failure;
    }
private:LanguageService &service_;std::optional<language::Id> last_;
};
class LocalizationCluster final:public chip::app::DefaultServerCluster {
public:
    explicit LocalizationCluster(LanguageService &service):DefaultServerCluster({0,0x2b}),service_(service){}
    void Refresh(){auto id=service_.Snapshot().observed;if(id!=last_){last_=id;NotifyAttributeChanged(0);}}
    CHIP_ERROR Attributes(const chip::app::ConcreteClusterPath &p,chip::ReadOnlyBufferBuilder<chip::app::DataModel::AttributeEntry> &b)override{
        ReturnErrorOnFailure(DefaultServerCluster::Attributes(p,b));using E=chip::app::DataModel::AttributeEntry;
        static const std::array<E,2> e{{{0,{},chip::Access::Privilege::kView,chip::Access::Privilege::kManage},{1,{},chip::Access::Privilege::kView,std::nullopt}}};return b.AppendElements(chip::Span(e));}
    chip::app::DataModel::ActionReturnStatus ReadAttribute(const chip::app::DataModel::ReadAttributeRequest &r,chip::app::AttributeValueEncoder &e)override{
        using namespace chip;const auto id=r.path.mAttributeId;
        if(id==0xfffc)return e.Encode<uint32_t>(0);if(id==0xfffd)return e.Encode<uint16_t>(1);
        if(id==0){auto v=service_.Snapshot().observed;if(v)return e.Encode(CharSpan::fromCharString(language::Locale(*v)));return Protocols::InteractionModel::Status::Failure;}
        if(id==1){std::array<CharSpan,2> locales{{CharSpan::fromCharString("en-US"),CharSpan::fromCharString("nl-NL")}};return e.Encode(app::DataModel::List<const CharSpan>(locales.data(),locales.size()));}
        return Protocols::InteractionModel::Status::UnsupportedAttribute;
    }
    chip::app::DataModel::ActionReturnStatus WriteAttribute(const chip::app::DataModel::WriteAttributeRequest &r,chip::app::AttributeValueDecoder &d)override{
        using namespace chip;if(r.path.mAttributeId!=0)return Protocols::InteractionModel::Status::UnsupportedWrite;
        CharSpan value;ReturnErrorOnFailure(d.Decode(value));auto id=language::FromLocale(std::string_view(value.data(),value.size()));
        if(!id)return Protocols::InteractionModel::Status::ConstraintError;
        return service_.Request(*id)?Protocols::InteractionModel::Status::Success:Protocols::InteractionModel::Status::Failure;
    }
private:LanguageService &service_;std::optional<language::Id> last_;
};
}
