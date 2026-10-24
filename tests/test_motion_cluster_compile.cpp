#include "../app/src/g2h_motion_cluster.hpp"
void VerifyMotionClusterInterface(g2h::MotionState &state){
    g2h::MotionCluster cluster(state);
    cluster.Refresh();
}
