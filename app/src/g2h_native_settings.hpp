#pragma once
#include "g2h_control_mode_cluster.hpp"
#include "g2h_vendor_control.hpp"
#include "g2h_video_endpoint.hpp"
#include <clusters/LevelControl/Commands.h>
#include <sstream>
#include <string>

namespace g2h {
// Additional controls are components of the existing node, never bridged devices.
class SettingsEndpoint {
public:
    SettingsEndpoint(uint16_t slot,chip::EndpointId endpoint,bool speaker=false):slot_(slot),id_(endpoint){types_[0]={speaker?0x0022u:0xFFF10001u,1};}
    CHIP_ERROR Register(){
        if(active_)return CHIP_ERROR_INCORRECT_STATE;
        const auto result=emberAfSetDynamicEndpoint(slot_,id_,&endpoint_,chip::Span(versions_),chip::Span<const EmberAfDeviceType>(types_),1);
        active_=result==CHIP_NO_ERROR;return result;
    }
    void Clear(){if(active_){emberAfClearDynamicEndpoint(slot_);active_=false;}}
private:
    inline static EmberAfAttributeMetadata attributes_[]={
        {ZAP_EMPTY_DEFAULT(),0,254,ZAP_TYPE(ARRAY),ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE)|ZAP_ATTRIBUTE_MASK(READABLE)},
        {ZAP_EMPTY_DEFAULT(),1,254,ZAP_TYPE(ARRAY),ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE)|ZAP_ATTRIBUTE_MASK(READABLE)},
        {ZAP_EMPTY_DEFAULT(),2,254,ZAP_TYPE(ARRAY),ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE)|ZAP_ATTRIBUTE_MASK(READABLE)},
        {ZAP_EMPTY_DEFAULT(),3,254,ZAP_TYPE(ARRAY),ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE)|ZAP_ATTRIBUTE_MASK(READABLE)},
        {ZAP_EMPTY_DEFAULT(),0xFFFC,4,ZAP_TYPE(BITMAP32),ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE)|ZAP_ATTRIBUTE_MASK(READABLE)},
        {ZAP_EMPTY_DEFAULT(),0xFFFD,2,ZAP_TYPE(INT16U),ZAP_ATTRIBUTE_MASK(EXTERNAL_STORAGE)|ZAP_ATTRIBUTE_MASK(READABLE)}};
    inline static EmberAfCluster clusters_[]={{chip::app::Clusters::Descriptor::Id,attributes_,MATTER_ARRAY_SIZE(attributes_),0,ZAP_CLUSTER_MASK(SERVER),nullptr,nullptr,nullptr}};
    inline static EmberAfEndpointType endpoint_={clusters_,MATTER_ARRAY_SIZE(clusters_),0};
    uint16_t slot_;chip::EndpointId id_;EmberAfDeviceType types_[1]{};chip::DataVersion versions_[1]{};bool active_=false;
};

class SettingModeCluster final:public chip::app::DefaultServerCluster {
public:
    using Observe=std::function<std::optional<int>()>;
    using Request=std::function<bool(int)>;
    SettingModeCluster(chip::EndpointId endpoint,const char *name,unsigned maximum,Observe observe,Request request):
        DefaultServerCluster({endpoint,0x50}),endpoint_(endpoint),name_(name),maximum_(maximum>2?2:maximum),observe_(std::move(observe)),request_(std::move(request)){}
    const char *Description()const{
        auto translated=language::ModeDescription(language::Current(),endpoint_);
        return translated?translated:name_;
    }
    std::optional<int> Observed()const{auto v=observe_();return v&&*v>=0&&unsigned(*v)<=maximum_?v:std::nullopt;}
    bool RequestMode(unsigned value){return value<=maximum_&&request_(int(value));}
    void Refresh(){
        const auto value=Observed();const auto locale=language::Current();
        if(locale!=lastLanguage_){
            lastLanguage_=locale;
            NotifyAttributeChanged(0);NotifyAttributeChanged(2);NotifyAttributeChanged(3);
        }else if(value!=last_)NotifyAttributeChanged(3);
        last_=value;
    }
    CHIP_ERROR Attributes(const chip::app::ConcreteClusterPath &p,chip::ReadOnlyBufferBuilder<chip::app::DataModel::AttributeEntry> &b)override{
        ReturnErrorOnFailure(DefaultServerCluster::Attributes(p,b));using E=chip::app::DataModel::AttributeEntry;
        static const std::array<E,4> e{{{0,{},chip::Access::Privilege::kView,std::nullopt},{1,{},chip::Access::Privilege::kView,std::nullopt},{2,{},chip::Access::Privilege::kView,std::nullopt},{3,{},chip::Access::Privilege::kView,std::nullopt}}};return b.AppendElements(chip::Span(e));
    }
    chip::app::DataModel::ActionReturnStatus ReadAttribute(const chip::app::DataModel::ReadAttributeRequest &r,chip::app::AttributeValueEncoder &e)override{
        using namespace chip;const auto id=r.path.mAttributeId;
        if(id==0xfffc)return e.Encode<uint32_t>(0);if(id==0xfffd)return e.Encode<uint16_t>(2);
        if(id==0)return e.Encode(CharSpan::fromCharString(Description()));
        if(id==1){app::DataModel::Nullable<uint16_t> v;return e.Encode(v);}
        if(id==2){using M=app::Clusters::ModeSelect::Structs::ModeOptionStruct::Type;std::array<M,3> modes{};
            const auto locale=language::Current();
            for(unsigned n=0;n<=maximum_&&n<modes.size();++n){modes[n].mode=n;modes[n].label=CharSpan::fromCharString(language::SettingOption(locale,n));}
            return e.Encode(app::DataModel::List<const M>(modes.data(),maximum_+1));}
        if(id==3){auto v=Observed();if(v)return e.Encode<uint8_t>(*v);return Protocols::InteractionModel::Status::Failure;}
        return Protocols::InteractionModel::Status::UnsupportedAttribute;
    }
    CHIP_ERROR AcceptedCommands(const chip::app::ConcreteClusterPath &,chip::ReadOnlyBufferBuilder<chip::app::DataModel::AcceptedCommandEntry> &b)override{
        using E=chip::app::DataModel::AcceptedCommandEntry;static const std::array<E,1> e{{{0,{},chip::Access::Privilege::kOperate}}};return b.AppendElements(chip::Span(e));}
    std::optional<chip::app::DataModel::ActionReturnStatus> InvokeCommand(const chip::app::DataModel::InvokeRequest &r,chip::TLV::TLVReader &a,chip::app::CommandHandler *)override{
        using namespace chip;if(r.path.mCommandId!=0)return Protocols::InteractionModel::Status::UnsupportedCommand;
        app::Clusters::ModeSelect::Commands::ChangeToMode::DecodableType d;ReturnErrorOnFailure(d.Decode(a));
        if(d.newMode>maximum_)return Protocols::InteractionModel::Status::InvalidCommand;
        return RequestMode(d.newMode)?Protocols::InteractionModel::Status::Success:Protocols::InteractionModel::Status::Failure;
    }
private:
    chip::EndpointId endpoint_;const char *name_;unsigned maximum_;Observe observe_;Request request_;std::optional<int> last_;
    language::Id lastLanguage_=language::Current();
};

// Real camera loudspeaker component. A level change never releases either mute.
class SpeakerLevelCluster final:public chip::app::DefaultServerCluster {
public:
    using Observe=std::function<std::optional<int>()>;using Request=std::function<bool(int)>;
    SpeakerLevelCluster(Observe observe,Request request):DefaultServerCluster({6,8}),observe_(std::move(observe)),request_(std::move(request)){}
    std::optional<int> Observed()const{auto v=observe_();return v&&*v>=0&&*v<=100?v:std::nullopt;}
    bool RequestLevel(unsigned value){return value<=100&&request_(int(value));}
    void Refresh(){auto v=Observed();if(v!=last_){last_=v;NotifyAttributeChanged(0);}}
    CHIP_ERROR Attributes(const chip::app::ConcreteClusterPath &p,chip::ReadOnlyBufferBuilder<chip::app::DataModel::AttributeEntry> &b)override{
        ReturnErrorOnFailure(DefaultServerCluster::Attributes(p,b));using E=chip::app::DataModel::AttributeEntry;
        static const std::array<E,5> e{{{0,{},chip::Access::Privilege::kView,std::nullopt},{1,{},chip::Access::Privilege::kView,std::nullopt},{2,{},chip::Access::Privilege::kView,std::nullopt},{3,{},chip::Access::Privilege::kView,std::nullopt},{15,{},chip::Access::Privilege::kView,chip::Access::Privilege::kOperate}}};return b.AppendElements(chip::Span(e));}
    chip::app::DataModel::ActionReturnStatus ReadAttribute(const chip::app::DataModel::ReadAttributeRequest &r,chip::app::AttributeValueEncoder &e)override{
        using namespace chip;const auto id=r.path.mAttributeId;
        if(id==0xfffc)return e.Encode<uint32_t>(0);if(id==0xfffd)return e.Encode<uint16_t>(6);
        if(id==0){app::DataModel::Nullable<uint8_t> v;auto n=Observed();if(n)v.SetNonNull(*n);return e.Encode(v);}
        if(id==1)return e.Encode<uint16_t>(0);if(id==2||id==15)return e.Encode<uint8_t>(0);if(id==3)return e.Encode<uint8_t>(100);
        return Protocols::InteractionModel::Status::UnsupportedAttribute;
    }
    chip::app::DataModel::ActionReturnStatus WriteAttribute(const chip::app::DataModel::WriteAttributeRequest &r,chip::app::AttributeValueDecoder &d)override{
        using namespace chip;if(r.path.mAttributeId!=15)return Protocols::InteractionModel::Status::UnsupportedWrite;
        uint8_t v=0;ReturnErrorOnFailure(d.Decode(v));return v==0?Protocols::InteractionModel::Status::Success:Protocols::InteractionModel::Status::ConstraintError;
    }
    CHIP_ERROR AcceptedCommands(const chip::app::ConcreteClusterPath &,chip::ReadOnlyBufferBuilder<chip::app::DataModel::AcceptedCommandEntry> &b)override{
        using E=chip::app::DataModel::AcceptedCommandEntry;static const std::array<E,2> e{{{0,{},chip::Access::Privilege::kOperate},{3,{},chip::Access::Privilege::kOperate}}};return b.AppendElements(chip::Span(e));}
    std::optional<chip::app::DataModel::ActionReturnStatus> InvokeCommand(const chip::app::DataModel::InvokeRequest &r,chip::TLV::TLVReader &a,chip::app::CommandHandler *)override{
        using namespace chip;
        if(r.path.mCommandId==3){app::Clusters::LevelControl::Commands::Stop::DecodableType d;ReturnErrorOnFailure(d.Decode(a));return Protocols::InteractionModel::Status::Success;}
        if(r.path.mCommandId!=0)return Protocols::InteractionModel::Status::UnsupportedCommand;
        app::Clusters::LevelControl::Commands::MoveToLevel::DecodableType d;ReturnErrorOnFailure(d.Decode(a));
        if(d.level>100)return Protocols::InteractionModel::Status::ConstraintError;
        if(!d.transitionTime.IsNull()&&d.transitionTime.Value()!=0)return Protocols::InteractionModel::Status::InvalidCommand;
        return RequestLevel(d.level)?Protocols::InteractionModel::Status::Success:Protocols::InteractionModel::Status::Failure;
    }
private:
    Observe observe_;Request request_;std::optional<int> last_;
};

inline std::optional<int8_t> ParseWlanRssi(const std::string &text){
    if(text.size()>4096)return {};std::istringstream lines(text);std::string line;
    while(std::getline(lines,line)){
        const auto colon=line.find(':');if(colon==std::string::npos)continue;
        if(line.substr(0,colon).find_first_not_of(" \t")==std::string::npos)continue;
        auto name=line.substr(0,colon);name.erase(0,name.find_first_not_of(" \t"));if(name!="wlan0")continue;
        std::istringstream fields(line.substr(colon+1));std::string status,quality,level;
        if(!(fields>>status>>quality>>level))return {};
        if(!level.empty()&&level.back()=='.')level.pop_back();
        if(level.size()<2||level.front()!='-'||level.size()>4)return {};
        int n=0;for(size_t i=1;i<level.size();++i){if(level[i]<'0'||level[i]>'9')return {};n=n*10+(level[i]-'0');}
        if(n<1||n>120)return {};return int8_t(-n);
    }return {};
}
class WifiSignalCluster final:public chip::app::DefaultServerCluster {
public:
    explicit WifiSignalCluster(std::function<std::optional<int8_t>()> observe):DefaultServerCluster({0,0x36}),observe_(std::move(observe)){}
    void Refresh(){auto v=observe_();if(v!=last_){last_=v;NotifyAttributeChanged(4);}}
    CHIP_ERROR Attributes(const chip::app::ConcreteClusterPath &p,chip::ReadOnlyBufferBuilder<chip::app::DataModel::AttributeEntry> &b)override{
        ReturnErrorOnFailure(DefaultServerCluster::Attributes(p,b));using E=chip::app::DataModel::AttributeEntry;static const std::array<E,1> e{{{4,{},chip::Access::Privilege::kView,std::nullopt}}};return b.AppendElements(chip::Span(e));}
    chip::app::DataModel::ActionReturnStatus ReadAttribute(const chip::app::DataModel::ReadAttributeRequest &r,chip::app::AttributeValueEncoder &e)override{
        using namespace chip;const auto id=r.path.mAttributeId;if(id==0xfffc)return e.Encode<uint32_t>(0);if(id==0xfffd)return e.Encode<uint16_t>(1);
        if(id!=4)return Protocols::InteractionModel::Status::UnsupportedAttribute;
        app::DataModel::Nullable<int8_t> v;auto n=observe_();if(n)v.SetNonNull(*n);return e.Encode(v);}
private:std::function<std::optional<int8_t>()> observe_;std::optional<int8_t> last_;
};
}
