#include "g2h_matter_signaling.hpp"
namespace g2h {
    CHIP_ERROR MatterSignaling::Send(const WebRtcDelegate::Target &target,const std::string &sdp,bool offer,std::function<void()> failed){
        if(!delegate_||!chip::IsOperationalNodeId(target.peer.GetNodeId())||target.peer.GetFabricIndex()==chip::kUndefinedFabricIndex||sdp.empty()||sdp.size()>16384)
            return CHIP_ERROR_INVALID_ARGUMENT;
        auto &server=chip::Server::GetInstance();
        if(!server.GetFabricTable().FindFabricWithIndex(target.peer.GetFabricIndex()))return CHIP_ERROR_NOT_FOUND;
        auto *manager=server.GetCASESessionManager();
        if(!manager)return CHIP_ERROR_INCORRECT_STATE;
        if(pending_)pending_->Cancel();
        pending_=std::make_shared<Pending>(target,sdp,offer,std::move(failed));
        // Retain while FindOrEstablishSession may invoke a callback synchronously.
        auto request=pending_;
        manager->FindOrEstablishSession(target.peer,&request->connected,&request->failed,chip::TransportPayloadCapability::kLargePayload);
        return CHIP_NO_ERROR;
    }
}
