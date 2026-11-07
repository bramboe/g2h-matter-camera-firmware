#include "../app/src/g2h_language_catalog.hpp"
#include <cassert>
#include <cstring>
#include <set>
#include <string>
using namespace g2h::language;
int main(){
    // Persisted/wire mode values are independent of the selected display text.
    for(unsigned n=0;n<kSupported.size();++n){
        const auto language=FromMode(n);assert(language&&unsigned(*language)==n);
        assert(FromLocale(Locale(*language))==language);
        assert(Name(*language)&&std::strlen(Name(*language)));
        for(unsigned key=0;key<kKeyCount;++key){
            const auto text=Text(*language,Key(key));assert(text&&std::strlen(text)>0);
            // Both Mode Select Description and option labels fit the 64-byte
            // bound; no encoding/truncation of a translated control is needed.
            assert(std::strlen(text)<=64);
        }
        std::set<std::string> options;
        for(unsigned mode=0;mode<3;++mode)assert(options.insert(SettingOption(*language,mode)).second);
        assert(std::strcmp(ControlOption(*language,false),ControlOption(*language,true)));
        for(uint16_t endpoint=0;endpoint<=5;++endpoint)assert(ModeDescription(*language,endpoint));
        assert(!ModeDescription(*language,6)); // Volume text belongs to HA.
        assert(!SettingOption(*language,3));
    }
    assert(!FromMode(2)&&!FromMode(256));
    assert(!FromLocale("nl")&&!FromLocale("nl-NL-extra")&&!FromLocale("de-DE"));
    assert(!Locale(Id(2))&&!Name(Id(2))&&!Text(Id(2),Key::Off));
    assert(!Text(Id::Dutch,Key::Count));
    // Default English stays byte-identical to already deployed mode labels.
    assert(!std::strcmp(ModeDescription(Id::English,3),"Night vision"));
    assert(!std::strcmp(ControlOption(Id::English,true),"Enabled"));
    assert(!std::strcmp(ControlOption(Id::Dutch,true),"Aan"));
    assert(!std::strcmp(SettingOption(Id::Dutch,2),"Automatisch"));
}
