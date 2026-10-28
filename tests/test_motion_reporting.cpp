#include "../app/src/g2h_motion_cluster.hpp"
#include <cassert>
#include <cstdio>
int main(){
    g2h::MotionState state;
    g2h::MotionCluster cluster(state);
    const chip::app::ConcreteClusterPath path{0,g2h::MotionCluster::kId};
    auto set=[&](std::optional<bool> value,const char *reason,std::optional<bool> enabled,std::uint64_t age=0){
        state.Set({value,g2h::motion_detail::Now()-age,reason,enabled});cluster.Refresh();
    };
    set(false,"native-held-motion",true);
    auto version=cluster.GetDataVersion(path);
    for(unsigned age=0;age<1000;age+=37){
        set(false,"native-held-motion",true,age);
        assert(cluster.GetDataVersion(path)==version); // Timing alone is not an event.
    }
    set(true,"native-held-motion",true);
    assert(cluster.GetDataVersion(path)==version+4);version=cluster.GetDataVersion(path);
    set(true,"native-held-motion",true,100);
    assert(cluster.GetDataVersion(path)==version);
    set(true,"native-held-motion",true,g2h::kMotionFreshnessMs+1);
    assert(cluster.GetDataVersion(path)==version+4);version=cluster.GetDataVersion(path);
    set(false,"native-held-motion",true);
    assert(cluster.GetDataVersion(path)==version+4);version=cluster.GetDataVersion(path);
    set({},"native-producer-disabled",false);
    assert(cluster.GetDataVersion(path)==version+4);version=cluster.GetDataVersion(path);
    set({},"native-producer-disabled",false,300);
    assert(cluster.GetDataVersion(path)==version);
    std::puts("MOTION_REPORTING_CHANGE_ONLY_OK");
}
