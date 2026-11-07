#pragma once
#include "g2h_adts.hpp"
#include "g2h_language_catalog.hpp"
#include <openssl/evp.h>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace g2h {
// Includes suffixes and is lexicographically ordered, matching SHA256SUMS.
inline constexpr std::array<const char*,17> kVoiceNames{{
    "add_ok.aac","add_sensor.aac","bind_ok.aac","bind_timeout.aac",
    "con_ok.aac","connecting.aac","deleted.aac","deleted_tip.aac",
    "fail_connect.aac","fail_join.aac","language_test.aac","leave_msg.aac",
    "leave_msg_fail.aac","reset_ok.aac","scan_success.aac","waiting.aac",
    "wifi_connected.aac"
}};
inline constexpr std::size_t kVoiceAssetLimit=192000,kVoiceManifestLimit=4096;

namespace voice_detail {
struct Fd {
    int n=-1;explicit Fd(int value=-1):n(value){}
    ~Fd(){if(n>=0)close(n);}
    Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;
};
inline bool Path(const std::string &path){
    if(path.size()<2||path.size()>4096||path.front()!='/'||path.back()=='/'||path.find('\0')!=path.npos)return false;
    std::size_t at=1;
    while(at<path.size()){
        const auto end=path.find('/',at);const auto part=path.substr(at,end==path.npos?end:end-at);
        if(part.empty()||part=="."||part=="..")return false;
        if(end==path.npos)break;at=end+1;
    }
    return true;
}
inline std::pair<std::string,std::string> Parent(const std::string &path){
    const auto at=path.rfind('/');return {at==0?"/":path.substr(0,at),path.substr(at+1)};
}
inline int Directory(const std::string &path){return open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);}
inline std::optional<std::string> LinkAt(int directory,const std::string &name){
    std::array<char,4097> bytes{};const auto n=readlinkat(directory,name.c_str(),bytes.data(),bytes.size());
    if(n<1||std::size_t(n)>=bytes.size())return {};return std::string(bytes.data(),std::size_t(n));
}
inline std::optional<std::string> Link(const std::string &path){
    if(!Path(path))return {};const auto parent=Parent(path);Fd directory(Directory(parent.first));
    return directory.n<0?std::nullopt:LinkAt(directory.n,parent.second);
}
inline bool Read(int directory,const char *name,std::size_t maximum,std::vector<std::uint8_t> &bytes){
    // O_NONBLOCK prevents a substituted FIFO from blocking before fstat.
    Fd fd(openat(directory,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));if(fd.n<0)return false;
    struct stat before{};if(fstat(fd.n,&before)||!S_ISREG(before.st_mode)||before.st_size<1||
        std::uint64_t(before.st_size)>maximum)return false;
    bytes.resize(std::size_t(before.st_size));std::size_t at=0;
    while(at<bytes.size()){
        const auto n=read(fd.n,bytes.data()+at,bytes.size()-at);
        if(n<0&&errno==EINTR)continue;if(n<=0)return false;at+=std::size_t(n);
    }
    unsigned char extra=0;ssize_t n;
    do{n=read(fd.n,&extra,1);}while(n<0&&errno==EINTR);
    struct stat after{};
    return n==0&&!fstat(fd.n,&after)&&S_ISREG(after.st_mode)&&before.st_dev==after.st_dev&&
        before.st_ino==after.st_ino&&before.st_size==after.st_size;
}
inline std::optional<std::string> Sha256(const std::vector<std::uint8_t> &bytes){
    EVP_MD_CTX *context=EVP_MD_CTX_new();if(!context)return {};
    unsigned char digest[EVP_MAX_MD_SIZE]{};unsigned size=0;
    const bool ok=EVP_DigestInit_ex(context,EVP_sha256(),nullptr)==1&&
        EVP_DigestUpdate(context,bytes.data(),bytes.size())==1&&
        EVP_DigestFinal_ex(context,digest,&size)==1&&size==32;
    EVP_MD_CTX_free(context);if(!ok)return {};
    static constexpr char hex[]="0123456789abcdef";std::string value;value.reserve(64);
    for(unsigned n=0;n<size;++n){value+=hex[digest[n]>>4];value+=hex[digest[n]&15];}return value;
}
}

// Owns only the configured current-bank pointer. It neither writes assets nor
// invokes any native camera operation. Call from the language service worker:
// complete asset validation intentionally performs bounded filesystem I/O.
class VoiceBanks {
public:
    using Sync=int(*)(int);
    VoiceBanks(std::string root="/mnt/sdcard/g2h-matter-camera/voice",
               std::string current="/mnt/config/g2h-matter-camera/voice-current",
               std::string alias="/etc/ch/en",Sync sync=::fsync):
        root_(std::move(root)),current_(std::move(current)),alias_(std::move(alias)),sync_(sync){}
    bool Valid()const{
        std::lock_guard<std::mutex> lock(mutex_);
        return Paths()&&BankValid(language::Id::English)&&BankValid(language::Id::Dutch)&&CurrentLocked().has_value();
    }
    // This is the exact configured mapping, not a repeated hash of every asset.
    // Valid() and Select() supply the separate complete asset-integrity checks.
    std::optional<language::Id> Current()const{
        std::lock_guard<std::mutex> lock(mutex_);return Paths()?CurrentLocked():std::nullopt;
    }
    bool Select(language::Id language){
        std::lock_guard<std::mutex> lock(mutex_);
        if(!Paths()||!sync_||!language::FromMode(static_cast<unsigned>(language))||!BankValid(language))return false;
        const auto old=CurrentLocked();if(!old)return false;
        if(*old==language)return true;
        const auto parent=voice_detail::Parent(current_);voice_detail::Fd directory(voice_detail::Directory(parent.first));
        if(directory.n<0)return false;
        const auto previous=Target(*old),target=Target(language),next=parent.second+".next";
        // Never remove or overwrite another operation's pre-existing stage.
        if(symlinkat(target.c_str(),directory.n,next.c_str()))return false;
        auto discard=[&]{(void)unlinkat(directory.n,next.c_str(),0);(void)sync_(directory.n);};
        if(sync_(directory.n)||voice_detail::LinkAt(directory.n,parent.second)!=previous||
            voice_detail::Link(alias_)!=std::optional<std::string>(current_)){
            discard();return false;
        }
        if(renameat(directory.n,next.c_str(),directory.n,parent.second.c_str())){discard();return false;}
        if(sync_(directory.n)==0&&CurrentLocked()==std::optional<language::Id>(language))return true;

        // Restore atomically if commit durability/readback failed. An external
        // replacement is not overwritten. Further filesystem failures can
        // defeat restoration; the caller must re-observe instead of publishing
        // a language on this false result.
        if(voice_detail::LinkAt(directory.n,parent.second)==std::optional<std::string>(target)&&
            symlinkat(previous.c_str(),directory.n,next.c_str())==0){
            if(renameat(directory.n,next.c_str(),directory.n,parent.second.c_str())==0)(void)sync_(directory.n);
            else discard();
        }
        return false;
    }
private:
    bool Paths()const{return voice_detail::Path(root_)&&voice_detail::Path(current_)&&voice_detail::Path(alias_);}
    std::string Target(language::Id language)const{return root_+(language==language::Id::English?"/en":"/nl");}
    std::optional<language::Id> CurrentLocked()const{
        if(voice_detail::Link(alias_)!=std::optional<std::string>(current_))return {};
        const auto target=voice_detail::Link(current_);if(!target)return {};
        if(*target==Target(language::Id::English))return language::Id::English;
        if(*target==Target(language::Id::Dutch))return language::Id::Dutch;
        return {};
    }
    bool BankValid(language::Id language)const{
        voice_detail::Fd root(voice_detail::Directory(root_));if(root.n<0)return false;
        voice_detail::Fd bank(openat(root.n,language==language::Id::English?"en":"nl",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
        if(bank.n<0)return false;
        std::vector<std::uint8_t> bytes;
        if(!voice_detail::Read(bank.n,"SHA256SUMS",kVoiceManifestLimit,bytes))return false;
        const std::string manifest(bytes.begin(),bytes.end());std::size_t at=0;
        for(const auto *name:kVoiceNames){
            const auto end=manifest.find('\n',at);if(end==manifest.npos)return false;
            const auto line=manifest.substr(at,end-at);const std::string suffix=std::string("  ")+name;
            if(line.size()!=64+suffix.size()||line.compare(64,suffix.size(),suffix)!=0)return false;
            const auto expected=line.substr(0,64);
            if(expected.find_first_not_of("0123456789abcdef")!=expected.npos)return false;
            if(!voice_detail::Read(bank.n,name,kVoiceAssetLimit,bytes)||!ValidAdts(bytes)||
                voice_detail::Sha256(bytes)!=std::optional<std::string>(expected))return false;
            at=end+1;
        }
        return at==manifest.size();
    }
    std::string root_,current_,alias_;Sync sync_;mutable std::mutex mutex_;
};
}
