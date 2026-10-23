#pragma once
#include <clusters/CameraAvStreamManagement/Commands.h>
namespace snapshotav=chip::app::Clusters::CameraAvStreamManagement;
class CameraSnapshotProof final:public ModelCommand {
public:
 CameraSnapshotProof(CredentialIssuerCommands *issuer):ModelCommand("snapshot",issuer){AddArguments();}
 System::Clock::Timeout GetWaitDuration() const override{return System::Clock::Seconds16(45);}
 CHIP_ERROR SendCommand(DeviceProxy *device,std::vector<EndpointId> endpoints) override{
  if(endpoints.size()!=1||endpoints[0]!=1)return CHIP_ERROR_INVALID_ARGUMENT;
  exchange_=device->GetExchangeManager();session_.Grab(device->GetSecureSession().Value());
  snapshotav::Commands::SnapshotStreamAllocate::Type command{};command.imageCodec=snapshotav::ImageCodecEnum::kJpeg;
  command.maxFrameRate=1;command.minResolution={320,180};command.maxResolution={320,180};command.quality=70;
  auto ok=[this](const ConcreteCommandPath&,const StatusIB&,const auto &response){cancel_={};id_=response.snapshotStreamID;Capture();};
  auto bad=[this](CHIP_ERROR error){cancel_={};Finish(error);};
  return Controller::InvokeCommandRequest(exchange_,session_.Get().Value(),1,command,ok,bad,NullOptional,MakeOptional(System::Clock::Timeout(10000)),&cancel_,true);
 }
 void Shutdown() override{if(cancel_)cancel_();cancel_={};session_.Release();ModelCommand::Shutdown();}
private:
 void Capture(){
  snapshotav::Commands::CaptureSnapshot::Type command{};command.snapshotStreamID.SetNonNull(id_);command.requestedResolution={320,180};
  auto ok=[this](const ConcreteCommandPath&,const StatusIB&,const auto &response){
   cancel_={};const auto &bytes=response.data;
   if(response.imageCodec!=snapshotav::ImageCodecEnum::kJpeg||response.resolution.width!=320||response.resolution.height!=180||
      bytes.size()<16||bytes.size()>64000||bytes[0]!=255||bytes[1]!=216||bytes[bytes.size()-2]!=255||bytes[bytes.size()-1]!=217){Finish(CHIP_ERROR_INVALID_ARGUMENT);return;}
   total_+=bytes.size();if(++count_<3)Capture();else Deallocate();
  };
  auto bad=[this](CHIP_ERROR error){cancel_={};Finish(error);};
  auto error=Controller::InvokeCommandRequest(exchange_,session_.Get().Value(),1,command,ok,bad,NullOptional,MakeOptional(System::Clock::Timeout(5000)),&cancel_,true);
  if(error!=CHIP_NO_ERROR)Finish(error);
 }
 void Deallocate(){
  snapshotav::Commands::SnapshotStreamDeallocate::Type command{};command.snapshotStreamID=id_;
  auto ok=[this](const ConcreteCommandPath&,const StatusIB&,const auto &){cancel_={};Finish(CHIP_NO_ERROR);};
  auto bad=[this](CHIP_ERROR error){cancel_={};Finish(error);};
  auto error=Controller::InvokeCommandRequest(exchange_,session_.Get().Value(),1,command,ok,bad,NullOptional,MakeOptional(System::Clock::Timeout(5000)),&cancel_,true);
  if(error!=CHIP_NO_ERROR)Finish(error);
 }
 void Finish(CHIP_ERROR error){if(finished_)return;finished_=true;
  printf("G2H_MATTER_SNAPSHOT {\"snapshots\":%u,\"bytes_total\":%zu,\"width\":320,\"height\":180,\"media_saved\":false,\"success\":%s}\n",count_,total_,error==CHIP_NO_ERROR?"true":"false");SetCommandExitStatus(error);
 }
 Messaging::ExchangeManager *exchange_=nullptr;SessionHolder session_;Controller::Internal::InvokeCancelFn cancel_;
 uint16_t id_=0;unsigned count_=0;size_t total_=0;bool finished_=false;
};
