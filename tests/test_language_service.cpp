#include "../app/src/g2h_language_service.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <thread>
namespace fs=std::filesystem;
using g2h::language::Id;
struct BanksFixture {
    fs::path path,root,current,alias;
    BanksFixture(){
        std::string pattern=(fs::temp_directory_path()/"g2h-language-service-XXXXXX").string();
        std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);assert(mkdtemp(name.data()));
        path=name.data();root=path/"voice";current=path/"config/voice-current";alias=path/"etc/ch/en";
        fs::create_directories(current.parent_path());fs::create_directories(alias.parent_path());
        const std::vector<std::uint8_t> aac{0xff,0xf1,0x60,0x40,0x01,0x1f,0xfc,0};
        auto hash=g2h::voice_detail::Sha256(aac);assert(hash);
        for(auto language:{"en","nl"}){
            fs::create_directories(root/language);std::ofstream manifest(root/language/"SHA256SUMS");
            for(auto file:g2h::kVoiceNames){
                std::ofstream out(root/language/file,std::ios::binary);out.write((const char*)aac.data(),aac.size());
                assert(out.good());manifest<<*hash<<"  "<<file<<'\n';
            }assert(manifest.good());
        }
        fs::create_symlink(root/"en",current);fs::create_symlink(current,alias);
    }
    ~BanksFixture(){std::error_code error;fs::remove_all(path,error);}
};
struct Native {
    std::string language="en";std::atomic<unsigned> reads{0},writes{0},failRead{0};std::atomic<bool> badAck{false};
    bool Exchange(const std::string &command,std::string &reply){
        if(command=="misc language"){
            const auto n=++reads;if(n==failRead)return false;reply=language+"\n";return true;
        }
        assert(command=="misc language en");++writes;
        if(badAck){reply="OK";return true;}language="en";reply="set misc_language en\n";return true;
    }
};
template<class Predicate> void Wait(Predicate predicate){
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!predicate()){assert(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(std::chrono::milliseconds(2));}
}
int main(){
    {
        BanksFixture f;Native native;
        g2h::LanguageService service([&](auto &c,auto &r){return native.Exchange(c,r);},f.root.string(),f.current.string(),f.alias.string());
        assert(!service.Request(Id::Dutch));assert(service.Start());
        Wait([&]{return service.Snapshot().observed==Id::English;});assert(native.writes==0);
        assert(!service.Request(Id(255)));assert(service.Request(Id::Dutch));
        Wait([&]{return service.Snapshot().observed==Id::Dutch;});
        assert(fs::read_symlink(f.current)==f.root/"nl"&&g2h::language::Current()==Id::Dutch&&native.writes==0);
        // A failed native read after changing the bank must restore the old bank
        // and leave the observed language unknown, never publish the request.
        native.failRead=native.reads+2;assert(service.Request(Id::English));
        Wait([&]{auto s=service.Snapshot();return !s.pending&&std::string(s.error)=="native-language-unconfirmed";});
        assert(!service.Snapshot().observed&&fs::read_symlink(f.current)==f.root/"nl");
        assert(g2h::language::Current()==Id::Dutch);service.Stop();
        // Selection survives the worker lifetime without a second preference.
        assert(service.Start());Wait([&]{return service.Snapshot().observed==Id::Dutch;});service.Stop();
    }
    {
        BanksFixture f;Native native;native.language="cn";native.badAck=true;
        g2h::LanguageService service([&](auto &c,auto &r){return native.Exchange(c,r);},f.root.string(),f.current.string(),f.alias.string());
        assert(service.Start());Wait([&]{return !service.Snapshot().pending;});
        assert(!service.Snapshot().observed&&native.writes==1&&fs::read_symlink(f.current)==f.root/"en");
        native.badAck=false;assert(service.Request(Id::Dutch));Wait([&]{return service.Snapshot().observed==Id::Dutch;});
        assert(native.writes==2&&native.language=="en");service.Stop();
    }
    {
        BanksFixture f;Native native;native.failRead=1;
        g2h::LanguageService service([&](auto &c,auto &r){return native.Exchange(c,r);},f.root.string(),f.current.string(),f.alias.string());
        assert(service.Start());Wait([&]{return !service.Snapshot().pending;});
        assert(!service.Snapshot().observed&&native.writes==0);service.Stop();
        fs::remove(f.root/"nl/add_ok.aac");assert(!service.Start());
    }
    puts("LANGUAGE_SERVICE_ASSERTIONS_OK");
}
