#include "../app/src/g2h_native_settings.hpp"
#include <cassert>
#include <cstdio>
int main(){
    using g2h::ParseWlanRssi;
    const std::string header="Inter-| sta-| Quality\n face | tus | link level noise\n";
    assert(ParseWlanRssi(header+" wlan0: 0000 96. -56. -256. 0 0\n")==-56);
    assert(!ParseWlanRssi(header+" wlan1: 0000 96. -56. -256. 0 0\n"));
    for(const char *bad:{"-256.","56.","-121.","-0.","-5x.","nan"})
        assert(!ParseWlanRssi(header+" wlan0: 0000 96. "+bad+" -256.\n"));
    assert(!ParseWlanRssi(std::string(4097,'x'))&&!ParseWlanRssi("garbage"));

    std::optional<int> native=2;int requested=-1;unsigned writes=0;
    g2h::SettingModeCluster night(3,"Night vision",2,[&]{return native;},[&](int v){requested=v;++writes;return true;});
    const chip::app::ConcreteClusterPath path{3,0x50};
    night.Refresh();auto version=night.GetDataVersion(path);
    for(int n=0;n<1000;++n)night.Refresh();assert(night.GetDataVersion(path)==version);
    assert(night.RequestMode(0)&&requested==0&&writes==1);
    assert(night.Observed()==2); // A queued command is not physical readback.
    assert(!night.RequestMode(3)&&writes==1);
    native.reset();night.Refresh();assert(night.GetDataVersion(path)==++version);
    native=0;night.Refresh();assert(night.GetDataVersion(path)==++version);
    native=99;assert(!night.Observed());

    std::optional<int> volume=80;writes=0;
    g2h::SpeakerLevelCluster speaker([&]{return volume;},[&](int v){requested=v;++writes;return true;});
    assert(speaker.RequestLevel(0)&&requested==0&&writes==1&&speaker.Observed()==80);
    assert(!speaker.RequestLevel(101)&&writes==1);
    volume=-1;assert(!speaker.Observed());volume=101;assert(!speaker.Observed());
    volume=61;speaker.Refresh();const chip::app::ConcreteClusterPath level{6,8};version=speaker.GetDataVersion(level);
    for(int n=0;n<1000;++n)speaker.Refresh();assert(speaker.GetDataVersion(level)==version);
    volume.reset();speaker.Refresh();assert(speaker.GetDataVersion(level)==++version);

    std::optional<int8_t> rssi=-56;
    g2h::WifiSignalCluster wifi([&]{return rssi;});const chip::app::ConcreteClusterPath signal{0,0x36};
    wifi.Refresh();version=wifi.GetDataVersion(signal);wifi.Refresh();assert(wifi.GetDataVersion(signal)==version);
    rssi=-57;wifi.Refresh();assert(wifi.GetDataVersion(signal)==++version);
    rssi.reset();wifi.Refresh();assert(wifi.GetDataVersion(signal)==++version);
    std::puts("NATIVE_SETTINGS_ASSERTIONS_OK");
}
