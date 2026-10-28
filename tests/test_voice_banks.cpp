#include "../app/src/g2h_voice_banks.hpp"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
namespace fs=std::filesystem;
using g2h::language::Id;
namespace {
unsigned syncCalls=0,failSyncAt=0;
int FaultSync(int fd){if(++syncCalls==failSyncAt){errno=EIO;return -1;}return fsync(fd);}
struct Fixture {
    fs::path path,root,current,alias;
    Fixture(){
        std::string pattern=(fs::temp_directory_path()/"g2h-voice-banks-XXXXXX").string();
        std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);assert(mkdtemp(name.data()));
        path=name.data();root=path/"voice";current=path/"config/voice-current";alias=path/"etc/ch/en";
        fs::create_directories(root/"en");fs::create_directories(root/"nl");
        fs::create_directories(current.parent_path());fs::create_directories(alias.parent_path());
        WriteBank("en");WriteBank("nl");fs::create_symlink(root/"en",current);fs::create_symlink(current,alias);
    }
    ~Fixture(){std::error_code error;fs::remove_all(path,error);}
    static std::vector<std::uint8_t> Aac(){return {0xff,0xf1,0x60,0x40,0x01,0x1f,0xfc,0};}
    static void Bytes(const fs::path &name,const std::vector<std::uint8_t> &bytes){
        std::ofstream out(name,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());assert(out.good());
    }
    static void Text(const fs::path &name,const std::string &text){std::ofstream out(name,std::ios::binary|std::ios::trunc);out<<text;assert(out.good());}
    std::string Manifest()const{
        auto hash=g2h::voice_detail::Sha256(Aac());assert(hash);std::string text;
        for(auto name:g2h::kVoiceNames)text+=*hash+"  "+name+"\n";return text;
    }
    void WriteBank(const char *language){
        for(auto name:g2h::kVoiceNames)Bytes(root/language/name,Aac());Text(root/language/"SHA256SUMS",Manifest());
    }
    g2h::VoiceBanks Banks(g2h::VoiceBanks::Sync sync=::fsync){return g2h::VoiceBanks(root.string(),current.string(),alias.string(),sync);}
    void EnUnchanged(){assert(fs::read_symlink(current)==root/"en");assert(!fs::exists(current.string()+".next"));}
};
}
int main(){
    static_assert(g2h::kVoiceNames.size()==17);
    for(std::size_t n=1;n<g2h::kVoiceNames.size();++n)
        assert(std::strcmp(g2h::kVoiceNames[n-1],g2h::kVoiceNames[n])<0);
    {
        Fixture f;auto banks=f.Banks();assert(banks.Valid()&&banks.Current()==Id::English);
        assert(banks.Select(Id::Dutch)&&banks.Current()==Id::Dutch&&banks.Valid());
        assert(fs::read_symlink(f.current)==f.root/"nl");
        assert(banks.Select(Id::English)&&banks.Select(Id::English));f.EnUnchanged();
        assert(!banks.Select(Id(255)));f.EnUnchanged();
    }
    {
        Fixture f;auto banks=f.Banks();auto bad=f.Manifest();bad[0]=bad[0]=='0'?'1':'0';
        Fixture::Text(f.root/"nl/SHA256SUMS",bad);
        assert(!banks.Valid()&&!banks.Select(Id::Dutch));f.EnUnchanged();
    }
    {
        Fixture f;auto banks=f.Banks();auto outside=f.path/"outside.aac";Fixture::Bytes(outside,Fixture::Aac());
        fs::remove(f.root/"nl/add_ok.aac");fs::create_symlink(outside,f.root/"nl/add_ok.aac");
        assert(!banks.Valid()&&!banks.Select(Id::Dutch));f.EnUnchanged();
    }
    {
        Fixture f;auto banks=f.Banks();auto bad=Fixture::Aac();bad[0]=0;
        Fixture::Bytes(f.root/"nl/add_ok.aac",bad);auto manifest=f.Manifest();
        // A matching digest must not admit bytes outside the guarded ADTS format.
        auto hash=g2h::voice_detail::Sha256(bad);assert(hash);manifest.replace(0,64,*hash);
        Fixture::Text(f.root/"nl/SHA256SUMS",manifest);
        assert(!banks.Valid()&&!banks.Select(Id::Dutch));f.EnUnchanged();
    }
    {
        Fixture f;auto banks=f.Banks();const auto file=f.root/"nl/add_ok.aac";fs::remove(file);
        assert(mkfifo(file.c_str(),0600)==0);
        assert(!banks.Valid()&&!banks.Select(Id::Dutch));f.EnUnchanged();
    }
    {
        Fixture f;auto banks=f.Banks();fs::remove_all(f.root/"nl");fs::create_directory_symlink(f.root/"en",f.root/"nl");
        assert(!banks.Valid()&&!banks.Select(Id::Dutch));f.EnUnchanged();
    }
    {
        Fixture f;auto banks=f.Banks();Fixture::Text(f.path/"outside-manifest",f.Manifest());
        fs::remove(f.root/"nl/SHA256SUMS");fs::create_symlink(f.path/"outside-manifest",f.root/"nl/SHA256SUMS");
        assert(!banks.Valid()&&!banks.Select(Id::Dutch));f.EnUnchanged();
    }
    for(bool oversizedManifest:{false,true}){
        Fixture f;auto banks=f.Banks();
        if(oversizedManifest)Fixture::Text(f.root/"nl/SHA256SUMS",std::string(g2h::kVoiceManifestLimit+1,'x'));
        else Fixture::Bytes(f.root/"nl/add_ok.aac",std::vector<std::uint8_t>(g2h::kVoiceAssetLimit+1,0));
        assert(!banks.Valid()&&!banks.Select(Id::Dutch));f.EnUnchanged();
    }
    {
        Fixture f;auto banks=f.Banks();fs::remove(f.alias);
        assert(!banks.Current()&&!banks.Valid()&&!banks.Select(Id::Dutch));f.EnUnchanged();
        fs::create_symlink("../config/voice-current",f.alias);assert(!banks.Current());
    }
    {
        Fixture f;auto banks=f.Banks();fs::remove(f.current);fs::create_symlink(f.root/"en/",f.current);
        assert(!banks.Current()&&!banks.Valid()&&!banks.Select(Id::Dutch));
    }
    for(const std::string suffix:{"\n",""}){
        Fixture f;auto banks=f.Banks();auto text=f.Manifest();
        if(suffix.empty())text.pop_back();else text+=suffix;
        Fixture::Text(f.root/"nl/SHA256SUMS",text);assert(!banks.Valid()&&!banks.Select(Id::Dutch));f.EnUnchanged();
    }
    {
        Fixture f;auto banks=f.Banks();auto text=f.Manifest();text[0]='A';Fixture::Text(f.root/"nl/SHA256SUMS",text);
        assert(!banks.Valid()&&!banks.Select(Id::Dutch));f.EnUnchanged();
    }
    {
        Fixture f;auto banks=f.Banks();const auto next=fs::path(f.current.string()+".next");Fixture::Text(next,"owned elsewhere");
        assert(!banks.Select(Id::Dutch)&&banks.Current()==Id::English);
        std::ifstream in(next);std::string text;std::getline(in,text);assert(text=="owned elsewhere");
    }
    // Both pre-rename and post-rename durability failures preserve/restore EN.
    for(unsigned fail:{1u,2u}){
        Fixture f;syncCalls=0;failSyncAt=fail;auto banks=f.Banks(FaultSync);
        assert(!banks.Select(Id::Dutch));f.EnUnchanged();assert(banks.Current()==Id::English&&banks.Valid());
    }
    {
        Fixture f;auto banks=f.Banks();fs::remove(f.root/"nl/add_ok.aac");fs::create_directory(f.root/"nl/add_ok.aac");
        assert(!banks.Valid()&&!banks.Select(Id::Dutch));f.EnUnchanged();
    }
    {
        Fixture f;auto banks=f.Banks();auto manifest=f.Manifest();
        const auto firstEnd=manifest.find('\n')+1,secondEnd=manifest.find('\n',firstEnd)+1;
        auto unsorted=manifest.substr(firstEnd,secondEnd-firstEnd)+manifest.substr(0,firstEnd)+manifest.substr(secondEnd);
        Fixture::Text(f.root/"nl/SHA256SUMS",unsorted);assert(!banks.Valid()&&!banks.Select(Id::Dutch));f.EnUnchanged();
    }
    std::puts("VOICE_BANKS_ASSERTIONS_OK");
}
