#include "../app/src/g2h_native_diagnostics.hpp"
#include <cassert>
#include <cstdio>
int main(){
    std::optional<bool> motion;
    g2h::NativeVisionMotion camera([&]{return motion;});
    const chip::app::ConcreteClusterPath path{1,g2h::NativeVisionMotion::kId};
    auto version=camera.GetDataVersion(path);
    camera.Refresh();assert(camera.GetDataVersion(path)==version);
    motion=false;camera.Refresh();assert(camera.GetDataVersion(path)==++version);
    for(unsigned n=0;n<1000;++n)camera.Refresh();
    assert(camera.GetDataVersion(path)==version); // No sample-age heartbeat spam.
    motion=true;camera.Refresh();assert(camera.GetDataVersion(path)==++version);
    motion.reset();camera.Refresh();assert(camera.GetDataVersion(path)==++version);
    camera.Refresh();assert(camera.GetDataVersion(path)==version);
    motion=false;camera.Refresh();assert(camera.GetDataVersion(path)==++version);

    g2h::RadioSnapshot sample;
    g2h::NativeThreadDiagnostics radio(g2h::NativeThreadDiagnostics::Observe([&]{return sample;}));
    const chip::app::ConcreteClusterPath rp{2,g2h::NativeThreadDiagnostics::kId};
    version=radio.GetDataVersion(rp);
    radio.Refresh();assert(radio.GetDataVersion(rp)==version);
    sample.valid=true;sample.sampled=std::chrono::steady_clock::now();
    sample.role="router";sample.channel=25;sample.network="fixture-thread";
    radio.Refresh();assert(radio.GetDataVersion(rp)==version+3);version+=3;
    for(unsigned n=0;n<1000;++n){sample.sampled=std::chrono::steady_clock::now();radio.Refresh();}
    assert(radio.GetDataVersion(rp)==version); // New samples are not changed values.
    sample.sampled-=std::chrono::seconds(91);radio.Refresh();
    assert(radio.GetDataVersion(rp)==version+3);version+=3;
    radio.Refresh();assert(radio.GetDataVersion(rp)==version);
    sample.valid=false;radio.Refresh();assert(radio.GetDataVersion(rp)==version);
    using R=g2h::NativeThreadDiagnostics;
    assert(R::RoutingRole("disabled")==0&&R::RoutingRole("detached")==1);
    assert(R::RoutingRole("child")==4&&R::RoutingRole("router")==5&&R::RoutingRole("leader")==6);
    assert(!R::RoutingRole("invalid")&&!R::RoutingRole(""));
    std::puts("NATIVE_DIAGNOSTICS_CHANGE_ONLY_OK");
}
