#include "../app/src/g2h_native_settings.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
int main(){
    assert(g2h::language::Current()==g2h::language::Id::English);
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

    // Publishing text only changes the actual cluster data version, never the
    // native request callback, endpoint, current setting or numeric mode IDs.
    native=0;night.Refresh();version=night.GetDataVersion(path);
    std::optional<bool> enabled=true;unsigned controlWrites=0;
    g2h::ControlModeCluster audio(1,"Camera audio",[&]{return enabled;},[&](bool){++controlWrites;return true;});
    const chip::app::ConcreteClusterPath audioPath{1,0x50};audio.Refresh();auto audioVersion=audio.GetDataVersion(audioPath);
    const auto beforeWrites=writes;
    assert(g2h::language::Publish(g2h::language::Id::Dutch));night.Refresh();audio.Refresh();
    assert(night.GetDataVersion(path)==version+3&&audio.GetDataVersion(audioPath)==audioVersion+3);
    version+=3;audioVersion+=3;
    assert(writes==beforeWrites&&controlWrites==0&&night.Observed()==0&&enabled==true);
    assert(!std::strcmp(night.Description(),"Nachtzicht")&&!std::strcmp(audio.Description(),"Camerageluid"));
    for(int n=0;n<1000;++n){night.Refresh();audio.Refresh();}
    assert(night.GetDataVersion(path)==version&&audio.GetDataVersion(audioPath)==audioVersion);
    assert(!g2h::language::Publish(g2h::language::Id(255))&&g2h::language::Current()==g2h::language::Id::Dutch);
    night.Refresh();audio.Refresh();assert(night.GetDataVersion(path)==version&&audio.GetDataVersion(audioPath)==audioVersion);
    assert(g2h::language::Publish(g2h::language::Id::English));night.Refresh();audio.Refresh();
    assert(night.GetDataVersion(path)==version+3&&audio.GetDataVersion(audioPath)==audioVersion+3);
    assert(!std::strcmp(night.Description(),"Night vision")&&!std::strcmp(audio.Description(),"Camera audio"));
    assert(writes==beforeWrites&&controlWrites==0);

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
