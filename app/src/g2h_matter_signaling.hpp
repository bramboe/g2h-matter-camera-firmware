#pragma once
#include "g2h_webrtc_delegate.hpp"
#include <app/CASESessionManager.h>
#include <app/server/Server.h>
#include <controller/InvokeInteraction.h>
#include <clusters/WebRTCTransportRequestor/Commands.h>
#include <platform/CHIPDeviceLayer.h>
#include <system/SystemLayer.h>

namespace g2h {
// Owned by the Linux app, used only on the Matter thread. Stop before the SDK
// server shuts down. Neither SDP nor ICE credentials are written to a log.
class MatterSignaling {
public:
    MatterSignaling()=default;
    ~MatterSignaling(){Stop();}
    MatterSignaling(const MatterSignaling&)=delete;
    MatterSignaling& operator=(const MatterSignaling&)=delete;
    void Attach(WebRtcDelegate &delegate){delegate_=&delegate;}
    CHIP_ERROR EnsurePolling(){
        if(!delegate_)return CHIP_ERROR_INCORRECT_STATE;
        if(polling_)return CHIP_NO_ERROR;
        auto err=chip::DeviceLayer::SystemLayer().StartTimer(chip::System::Clock::Milliseconds32(50),Tick,this);
        if(err==CHIP_NO_ERROR)polling_=true;
        return err;
    }
    CHIP_ERROR Send(const WebRtcDelegate::Target &target,const std::string &sdp,bool offer,std::function<void()> failed);
    void Cancel(const WebRtcDelegate::Target &target){
        if(pending_&&pending_->target.epoch==target.epoch&&pending_->target.peer==target.peer){pending_->Cancel();pending_.reset();}
    }
    void Stop(){
        if(polling_){chip::DeviceLayer::SystemLayer().CancelTimer(Tick,this);polling_=false;}
        if(pending_){pending_->Cancel();pending_.reset();}
        delegate_=nullptr;
    }
private:
    struct Pending : std::enable_shared_from_this<Pending> {
        WebRtcDelegate::Target target;
        std::string payload;
        bool offer,done=false;
        std::function<void()> failure;
        chip::Controller::Internal::InvokeCancelFn cancelInvoke;
        chip::Callback::Callback<chip::OnDeviceConnected> connected;
        chip::Callback::Callback<chip::OnDeviceConnectionFailure> failed;
        Pending(const WebRtcDelegate::Target &t,const std::string &sdp,bool o,std::function<void()> f)
            :target(t),payload(sdp),offer(o),failure(std::move(f)),connected(Connected,this),failed(Failed,this){}
        ~Pending(){Cancel();}
        void Cancel() noexcept {
            connected.Cancel();failed.Cancel();done=true;
            auto cancel=std::move(cancelInvoke);cancelInvoke={};
            try {if(cancel)cancel();}catch(...){}
            payload.clear();failure={};
        }
        void Fail(){if(!done){done=true;cancelInvoke={};payload.clear();auto callback=std::move(failure);if(callback)callback();}}
        static void Failed(void *context,const chip::ScopedNodeId&,CHIP_ERROR){
            auto self=static_cast<Pending*>(context)->shared_from_this();self->Fail();
        }
        static void Connected(void *context,chip::Messaging::ExchangeManager &exchange,const chip::SessionHandle &session){
            auto self=static_cast<Pending*>(context)->shared_from_this();
            if(self->done)return;
            // CASE selection alone is not a reason to skip the identity check.
            if(!session->IsSecureSession()||!session->AsSecureSession()->IsCASESession()||session->GetPeer()!=self->target.peer){self->Fail();return;}
            auto weak=std::weak_ptr<Pending>(self);
            auto success=[weak](const chip::app::ConcreteCommandPath&,const chip::app::StatusIB&,const auto&){
                if(auto p=weak.lock()){p->done=true;p->cancelInvoke={};p->payload.clear();p->failure={};}
            };
            auto failure=[weak](CHIP_ERROR){if(auto p=weak.lock())p->Fail();};
            CHIP_ERROR err;
            if(self->offer){
                chip::app::Clusters::WebRTCTransportRequestor::Commands::Offer::Type command;
                command.webRTCSessionID=self->target.session;command.sdp=chip::CharSpan(self->payload.data(),self->payload.size());
                err=chip::Controller::InvokeCommandRequest(&exchange,session,self->target.endpoint,command,success,failure,
                    chip::NullOptional,chip::MakeOptional(chip::System::Clock::Timeout(10000)),&self->cancelInvoke,true);
            }else{
                chip::app::Clusters::WebRTCTransportRequestor::Commands::Answer::Type command;
                command.webRTCSessionID=self->target.session;command.sdp=chip::CharSpan(self->payload.data(),self->payload.size());
                err=chip::Controller::InvokeCommandRequest(&exchange,session,self->target.endpoint,command,success,failure,
                    chip::NullOptional,chip::MakeOptional(chip::System::Clock::Timeout(10000)),&self->cancelInvoke,true);
            }
            if(err!=CHIP_NO_ERROR)self->Fail();
        }
    };
    static void Tick(chip::System::Layer*,void *context){
        auto self=static_cast<MatterSignaling*>(context);self->polling_=false;
        if(!self->delegate_)return;
        try {self->delegate_->Poll();}catch(...){self->delegate_->Shutdown();}
        if(!self->delegate_->HasSession())return;
        if(self->EnsurePolling()!=CHIP_NO_ERROR)self->delegate_->Shutdown();
    }
    WebRtcDelegate *delegate_=nullptr;
    bool polling_=false;
    std::shared_ptr<Pending> pending_;
};
}
