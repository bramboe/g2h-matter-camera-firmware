#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace g2h::language {
// Stable wire/storage values: changing display text never renumbers a mode.
enum class Id : uint8_t { English=0, Dutch=1 };
inline constexpr std::array<Id,2> kSupported{{Id::English,Id::Dutch}};
inline constexpr const char *kSelectorDescription="Language / Taal";

constexpr std::optional<Id> FromMode(unsigned mode){
    if(mode==0)return Id::English;
    if(mode==1)return Id::Dutch;
    return {};
}
namespace detail { inline std::atomic<Id> published{Id::English}; }
inline Id Current() noexcept {return detail::published.load(std::memory_order_acquire);}
// The service publishes only after durable preference/backend acknowledgement.
// Invalid stored or wire values cannot poison descriptions or option labels.
inline bool Publish(Id language) noexcept {
    if(!FromMode(static_cast<unsigned>(language)))return false;
    detail::published.store(language,std::memory_order_release);return true;
}
constexpr std::optional<Id> FromLocale(std::string_view locale){
    if(locale=="en-US")return Id::English;
    if(locale=="nl-NL")return Id::Dutch;
    return {};
}
constexpr const char *Locale(Id language){
    return language==Id::English?"en-US":language==Id::Dutch?"nl-NL":nullptr;
}
// Endonyms keep the language selector understandable after either choice.
constexpr const char *Name(Id language){
    return language==Id::English?"English":language==Id::Dutch?"Nederlands":nullptr;
}

enum class Key : uint8_t {
    CameraAudio,CameraVideoRestart,ThreadRadio,NightVision,StatusLight,
    DateTimeOverlay,Disabled,Enabled,Off,On,Automatic,Count
};
inline constexpr std::size_t kKeyCount=static_cast<std::size_t>(Key::Count);
inline constexpr std::array<std::array<const char*,kKeyCount>,2> kText{{
    {{"Camera audio","Camera video (restart)","Thread radio","Night vision",
      "Status light","Date and time overlay","Disabled","Enabled",
      "Off","On","Automatic"}},
    {{"Camerageluid","Camerabeeld (herstart)","Thread-radio","Nachtzicht",
      "Statuslamp","Datum en tijd in beeld","Uit","Aan",
      "Uit","Aan","Automatisch"}}
}};
constexpr const char *Text(Id language,Key key){
    const auto row=static_cast<std::size_t>(language),column=static_cast<std::size_t>(key);
    return row<kText.size()&&column<kKeyCount?kText[row][column]:nullptr;
}
// Only these firmware-provided Mode Select descriptions are under our control.
// HA's generic volume/diagnostic names and user registry overrides are not.
constexpr const char *ModeDescription(Id language,uint16_t endpoint){
    switch(endpoint){
        case 0:return Text(language,Key::ThreadRadio);
        case 1:return Text(language,Key::CameraAudio);
        case 2:return Text(language,Key::CameraVideoRestart);
        case 3:return Text(language,Key::NightVision);
        case 4:return Text(language,Key::StatusLight);
        case 5:return Text(language,Key::DateTimeOverlay);
        default:return nullptr;
    }
}
constexpr const char *ControlOption(Id language,bool enabled){
    return Text(language,enabled?Key::Enabled:Key::Disabled);
}
constexpr const char *SettingOption(Id language,unsigned mode){
    if(mode==0)return Text(language,Key::Off);
    if(mode==1)return Text(language,Key::On);
    if(mode==2)return Text(language,Key::Automatic);
    return nullptr;
}
}
