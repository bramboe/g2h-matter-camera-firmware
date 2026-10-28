#pragma once
// Read-only candidate observer. Native HomeKit held-motion is not occupancy.
// No ptrace attach, process signal, settings write, UART or GPIO operation.
#include <openssl/evp.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace g2h {
// One-second producer cadence plus a two-second read deadline and scheduling margin.
inline constexpr std::uint16_t kMotionFreshnessMs=4000;
namespace motion_detail {
inline constexpr std::uint32_t kValue=0x559fa8,kEnabled=0x559fc5;
inline constexpr const char *kSha="f942639021769407028aa41e1064d794dd86f07ffab1442a91112c2aa678c799";
struct Fd{int n=-1;explicit Fd(int f=-1):n(f){}~Fd(){if(n>=0)close(n);}Fd(const Fd&)=delete;Fd&operator=(const Fd&)=delete;};
inline std::uint64_t Now(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline bool ReadText(int directory,const char *name,std::string &out,std::size_t max){
    Fd fd(openat(directory,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW));if(fd.n<0)return false;
    out.clear();std::array<char,1024>b{};
    for(;;){auto n=read(fd.n,b.data(),b.size());if(n<0)return false;if(!n)return true;if(out.size()+std::size_t(n)>max)return false;out.append(b.data(),std::size_t(n));}
}
inline std::optional<std::uint64_t> StartTicks(const std::string &text,pid_t pid){
    auto end=text.rfind(") ");auto begin=text.find(" (");
    if(begin==std::string::npos||end==std::string::npos||text.substr(0,begin)!=std::to_string(pid))return {};
    std::istringstream fields(text.substr(end+2));std::string value;
    for(unsigned i=0;i<20;++i)if(!(fields>>value))return {};
    if(value.empty()||value.size()>20||value.find_first_not_of("0123456789")!=std::string::npos)return {};
    char *tail=nullptr;errno=0;auto result=std::strtoull(value.c_str(),&tail,10);
    if(errno||!tail||*tail||!result)return {};return result;
}
inline bool RootUids(const std::string &text){
    auto at=text.find("Uid:");if(at==std::string::npos||(at&&text[at-1]!='\n'))return false;
    std::istringstream fields(text.substr(at+4));unsigned long ids[4]{};
    for(auto &id:ids)if(!(fields>>id)||id)return false;return true;
}
inline std::array<std::int64_t,4> Times(const struct stat &s){
#ifdef __APPLE__
    return {s.st_mtimespec.tv_sec,s.st_mtimespec.tv_nsec,s.st_ctimespec.tv_sec,s.st_ctimespec.tv_nsec};
#else
    return {s.st_mtim.tv_sec,s.st_mtim.tv_nsec,s.st_ctim.tv_sec,s.st_ctim.tv_nsec};
#endif
}
struct Identity{
    pid_t pid=0;std::uint64_t born=0;struct stat image{};
    bool operator==(const Identity&o)const{return pid==o.pid&&born==o.born&&image.st_dev==o.image.st_dev&&image.st_ino==o.image.st_ino&&image.st_size==o.image.st_size&&image.st_mode==o.image.st_mode&&image.st_uid==o.image.st_uid&&Times(image)==Times(o.image);}
};
inline bool Mapping(const std::string &maps,std::uint32_t address,std::size_t length){
    std::istringstream lines(maps);std::string line;
    while(std::getline(lines,line)){unsigned long long first=0,last=0;char mode[5]{};
        if(std::sscanf(line.c_str(),"%llx-%llx %4s",&first,&last,mode)==3&&first<=address&&std::uint64_t(address)+length<=last&&
            mode[0]=='r'&&mode[1]=='w'&&(mode[2]=='-'||mode[2]=='x')&&mode[3]=='p')return true;}
    return false;
}
enum class HashStatus{Success,ReadFailed,Timeout,TooLarge,CryptoFailure};
inline HashStatus HashDetailed(int fd,std::string &hex,std::uint64_t budgetMs=12000){
    EVP_MD_CTX *ctx=EVP_MD_CTX_new();if(!ctx)return HashStatus::CryptoFailure;
    bool ok=EVP_DigestInit_ex(ctx,EVP_sha256(),nullptr)==1;
    HashStatus status=HashStatus::Success;
    std::array<unsigned char,65536> block{};std::size_t offset=0;auto until=Now()+budgetMs;
    while(ok){
        if(Now()>=until){ok=false;status=HashStatus::Timeout;break;}
        auto n=pread(fd,block.data(),block.size(),off_t(offset));if(n<0){ok=false;status=HashStatus::ReadFailed;break;}if(!n)break;
        offset+=std::size_t(n);if(offset>32*1024*1024){ok=false;status=HashStatus::TooLarge;break;}ok=EVP_DigestUpdate(ctx,block.data(),std::size_t(n))==1;}
    unsigned char digest[EVP_MAX_MD_SIZE]{};unsigned size=0;
    if(ok)ok=EVP_DigestFinal_ex(ctx,digest,&size)==1&&size==32;
    EVP_MD_CTX_free(ctx);if(!ok)return status==HashStatus::Success?HashStatus::CryptoFailure:status;
    hex.clear();static constexpr char chars[]="0123456789abcdef";
    for(unsigned i=0;i<size;++i){hex+=chars[digest[i]>>4];hex+=chars[digest[i]&15];}return HashStatus::Success;
}
inline bool Hash(int fd,std::string &hex){return HashDetailed(fd,hex)==HashStatus::Success;}
inline std::uint32_t U32(const unsigned char*p){return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);}
inline bool ElfLayout(int fd,off_t size){
    std::array<unsigned char,52> h{};if(pread(fd,h.data(),h.size(),0)!=ssize_t(h.size()))return false;
    if(std::memcmp(h.data(),"\177ELF\1\1\1",7)||h[16]!=2||h[17]||h[18]!=40||h[19])return false;
    auto phoff=U32(h.data()+28);unsigned count=unsigned(h[44])|(unsigned(h[45])<<8);
    if(h[42]!=32||h[43]||!count||count>32||std::uint64_t(phoff)+count*32>std::uint64_t(size))return false;
    bool value=false,enabled=false;
    for(unsigned i=0;i<count;++i){unsigned char p[32]{};if(pread(fd,p,32,phoff+i*32)!=32)return false;
        auto start=U32(p+8),bytes=U32(p+20),flags=U32(p+24);
        if(U32(p)!=1||(flags&6)!=6)continue;
        auto covers=[&](std::uint32_t a,unsigned n){return start<=a&&std::uint64_t(a)+n<=std::uint64_t(start)+bytes;};
        value|=covers(kValue,4);enabled|=covers(kEnabled,1);}
    return value&&enabled;
}
}

struct MotionObservation{
    std::optional<bool> heldMotion;std::uint64_t sampledMs=0;const char *reason="not-sampled";
    std::optional<bool> producerEnabled;
    std::optional<bool> Value(std::uint64_t nowMs=motion_detail::Now())const{
        if(!heldMotion||nowMs<sampledMs||nowMs-sampledMs>kMotionFreshnessMs)return {};return heldMotion;
    }
    std::optional<bool> Enabled(std::uint64_t nowMs=motion_detail::Now())const{
        if(!sampledMs||nowMs<sampledMs||nowMs-sampledMs>kMotionFreshnessMs)return {};return producerEnabled;
    }
};

class MotionState{
public:
    void Set(const MotionObservation &value){std::lock_guard<std::mutex> lock(mutex_);value_=value;}
    MotionObservation Snapshot()const{std::lock_guard<std::mutex> lock(mutex_);return value_;}
private:mutable std::mutex mutex_;MotionObservation value_;
};

// Poll from a dedicated bounded worker, at most once per second. The first poll
// per executable identity hashes at most32MiB; later polls do not rehash it.
// This class deliberately exposes no setting to enable the native producer.
class MotionObserver{
public:
    MotionObserver()=default;
#ifdef G2H_MOTION_TEST
    MotionObserver(std::string proc,std::string exe,std::string sha,uid_t owner):proc_(std::move(proc)),exe_(std::move(exe)),sha_(std::move(sha)),owner_(owner){}
    unsigned HashAttempts()const{return hashes_;}
#endif
    MotionObservation Observe(pid_t pid){
        using namespace motion_detail;
        auto fail=[](const char *reason){return MotionObservation{{},0,reason,{}};};
        if(pid<2)return fail("invalid-pid");
        Fd proc(open((proc_+"/"+std::to_string(pid)).c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
        if(proc.n<0)return fail("process-unavailable");
        motion_detail::Identity before;Fd image(openat(proc.n,"exe",O_RDONLY|O_CLOEXEC));
        if(image.n<0||!ReadIdentity(proc.n,image.n,pid,before))return fail("identity-unavailable");
        if(!verified_||!(*verified_==before)){
            if(rejected_&&*rejected_==before)return fail(rejectedReason_);
            if(retry_&&*retry_==before&&Now()<retryAfterMs_)return fail(retryReason_);
            verified_.reset();++hashes_;std::string hash;
            if(!ElfLayout(image.n,before.image.st_size)){rejected_=before;rejectedReason_="executable-layout-mismatch";return fail(rejectedReason_);}
            const auto hashed=HashDetailed(image.n,hash);
            if(hashed!=HashStatus::Success){
                retryReason_=hashed==HashStatus::Timeout?"hash-timeout":hashed==HashStatus::ReadFailed?"hash-read-failed":hashed==HashStatus::TooLarge?"hash-size-limit":"hash-crypto-failed";
                retry_=before;retryAfterMs_=Now()+30000;return fail(retryReason_);
            }
            if(hash!=sha_){rejected_=before;rejectedReason_="executable-hash-mismatch";return fail(rejectedReason_);}
            struct stat afterHash{};if(fstat(image.n,&afterHash))return fail("identity-unavailable");
            motion_detail::Identity checked=before;checked.image=afterHash;
            if(!(checked==before))return fail("executable-changed");
            verified_=before;rejected_.reset();retry_.reset();
        }
        std::string maps;if(!ReadText(proc.n,"maps",maps,32768)||!Mapping(maps,kValue,4)||!Mapping(maps,kEnabled,1))return fail("mapping-unavailable");
        // Only these two proven fields are read. Never read surrounding memory.
        Fd memory(openat(proc.n,"mem",O_RDONLY|O_CLOEXEC|O_NOFOLLOW));
        if(memory.n<0)return fail("memory-permission-denied");
        auto began=Now();unsigned char first=0,last=0,value[4]{};
        if(pread(memory.n,&first,1,kEnabled)!=1||pread(memory.n,value,4,kValue)!=4||pread(memory.n,&last,1,kEnabled)!=1)return fail("memory-read-failed");
        motion_detail::Identity after;Fd afterImage(openat(proc.n,"exe",O_RDONLY|O_CLOEXEC));
        if(afterImage.n<0||!ReadIdentity(proc.n,afterImage.n,pid,after)||!(before==after))return fail("process-changed");
        auto ended=Now();if(ended<began||ended-began>1000)return fail("sample-expired");
        if(first!=last||first>1)return fail("invalid-native-enable");
        if(first==0)return {{},ended,"native-producer-disabled",false};
        auto held=U32(value);if(held>1)return fail("invalid-native-value");
        return {held==1,ended,"native-held-motion",true};
    }
    std::optional<pid_t> FindVendorPid()const{
        DIR *dir=opendir(proc_.c_str());if(!dir)return {};
        std::optional<pid_t> found;unsigned count=0;bool valid=true;
        while(auto entry=readdir(dir)){
            if(++count>4096){valid=false;break;}
            const std::string name=entry->d_name;if(name.empty()||name.size()>10||name.find_first_not_of("0123456789")!=std::string::npos)continue;
            auto number=std::strtoul(name.c_str(),nullptr,10);if(number<2||number>0x7fffffff)continue;
            motion_detail::Fd proc(open((proc_+"/"+name).c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));if(proc.n<0)continue;
            std::string comm;char path[512]{};auto n=readlinkat(proc.n,"exe",path,sizeof(path)-1);
            if(n<0||std::string(path,std::size_t(n))!=exe_||!motion_detail::ReadText(proc.n,"comm",comm,64)||comm!="camera\n")continue;
            if(found){valid=false;break;}found=pid_t(number);
        }
        closedir(dir);return valid?found:std::optional<pid_t>{};
    }
private:
    bool ReadIdentity(int proc,int image,pid_t pid,motion_detail::Identity &out)const{
        std::string comm,status,stat;char path[512]{};struct stat owner{};
        auto n=readlinkat(proc,"exe",path,sizeof(path)-1);
        if(n<0||std::string(path,std::size_t(n))!=exe_||fstat(proc,&owner)||owner.st_uid!=owner_)return false;
        if(!motion_detail::ReadText(proc,"comm",comm,64)||comm!="camera\n"||
           !motion_detail::ReadText(proc,"status",status,4096)||!motion_detail::RootUids(status)||
           !motion_detail::ReadText(proc,"stat",stat,4096))return false;
        auto born=motion_detail::StartTicks(stat,pid);if(!born||fstat(image,&out.image)||!S_ISREG(out.image.st_mode)||out.image.st_uid!=owner_||
            (out.image.st_mode&0022)||out.image.st_size<52||out.image.st_size>32*1024*1024)return false;
        out.pid=pid;out.born=*born;return true;
    }
    std::string proc_="/proc",exe_="/tmp/out/camera",sha_=motion_detail::kSha;uid_t owner_=0;
    std::optional<motion_detail::Identity> verified_,rejected_,retry_;unsigned hashes_=0;
    const char *rejectedReason_="untrusted-executable",*retryReason_="hash-unavailable";std::uint64_t retryAfterMs_=0;
};
}
