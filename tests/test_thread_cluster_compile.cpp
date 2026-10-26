#include "../app/src/g2h_thread_cluster.hpp"
static_assert(g2h::ThreadRadioCluster::kRequestedEnabled==24);
static_assert(g2h::ThreadRadioCluster::kObservedEnabled==25);
void VerifyThreadClusterInterface(g2h::ThreadRadio &radio){
    g2h::ThreadRadioCluster runtimeOnly(radio);
    g2h::ThreadRadioCluster persistent(radio,[](bool){return true;});
    runtimeOnly.Refresh();persistent.Refresh();
}
