#pragma once
#include <chrono>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
namespace g2h {
// No counter reset or forwarding inference: all counters are raw radio totals.
struct RadioDataset {
    bool valid=false; unsigned count=0;
    std::string table; // Validated CLI table, bounded; never contains datasets/keys.
    std::chrono::steady_clock::time_point sampled{};
    bool Fresh() const {return valid && std::chrono::steady_clock::now()-sampled<std::chrono::seconds(90);}
};
struct RadioCounters {
    bool valid=false; uint32_t tx=0,rx=0,retry=0; uint64_t uptimeMs=0;
    std::chrono::steady_clock::time_point sampled{};
    bool Fresh() const {return valid && std::chrono::steady_clock::now()-sampled<std::chrono::seconds(90);}
};
inline std::string RadioTrim(std::string s){auto a=s.find_first_not_of(" \t\r");return a==s.npos?"":s.substr(a,s.find_last_not_of(" \t\r")-a+1);}
inline bool RadioUnsigned(const std::string &s,uint64_t maximum,uint64_t &out,unsigned base=10){
    auto text=s;if(base==16&&text.rfind("0x",0)==0)text.erase(0,2);
    if(text.empty()||text.size()>20)return false;uint64_t n=0;
    for(char c:text){unsigned d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:99;
        if(d>=base||d>maximum||n>(maximum-d)/base)return false;n=n*base+d;}
    out=n;return true;
}
inline std::vector<std::string> RadioCells(const std::string &line){
    std::vector<std::string> c;if(line.empty()||line.front()!='|'||line.back()!='|')return c;
    size_t p=1,n;while((n=line.find('|',p))!=line.npos){c.push_back(RadioTrim(line.substr(p,n-p)));p=n+1;}return c;
}
inline bool ParseRadioTable(const std::vector<std::string> &lines,const std::string &kind,RadioDataset &out){
    // Match real OpenThread CLI headers, including versions with optional columns.
    if(lines.size()<2||lines.size()>66)return false;
    auto header=RadioCells(lines[0]);if(header.empty())return false;
    std::map<std::string,size_t> index;for(size_t i=0;i<header.size();++i)if(!index.emplace(header[i],i).second)return false;
    const std::vector<std::string> required=kind=="neighbor"?std::vector<std::string>{"Role","RLOC16","Age","Avg RSSI","Last RSSI","Extended MAC"}:
        kind=="child"?std::vector<std::string>{"ID","RLOC16","Timeout","Age","LQ In","Extended MAC"}:
        kind=="router"?std::vector<std::string>{"ID","RLOC16","Next Hop","Path Cost","LQ In","LQ Out","Age","Extended MAC","Link"}:std::vector<std::string>{};
    if(required.empty())return false;for(auto &key:required)if(!index.count(key))return false;
    if(lines[1].empty()||lines[1].front()!='+'||lines[1].back()!='+'||lines[1].find_first_not_of("+-")!=lines[1].npos)return false;
    std::string text=lines[0]+"\n"+lines[1];unsigned count=0;
    for(size_t i=2;i<lines.size();++i){auto cells=RadioCells(lines[i]);if(cells.size()!=header.size())return false;
        for(size_t j=0;j<cells.size();++j){auto &name=header[j];auto &v=cells[j];uint64_t n;
            if(name=="Extended MAC"){if(v.size()!=16)return false;for(char c:v)if(!isxdigit(static_cast<unsigned char>(c)))return false;}
            else if(name=="Role"){if(v!="C"&&v!="R")return false;}
            else if(name=="Avg RSSI"||name=="Last RSSI"){bool neg=!v.empty()&&v[0]=='-';if(!RadioUnsigned(neg?v.substr(1):v,neg?128:127,n))return false;}
            else if(!RadioUnsigned(v,name=="RLOC16"?65535:UINT32_MAX,n,name=="RLOC16"?16:10))return false;
            if((name=="LQ In"||name=="LQ Out")&&n>3)return false;
            if((name=="R"||name=="D"||name=="N"||name=="Link"||name=="CSL")&&n>1)return false;
            if(((name=="ID"&&kind=="router")||name=="Next Hop")&&n>63)return false;
        }
        text+='\n'+lines[i];if(text.size()>12288)return false;++count;
    }
    out={true,count,std::move(text),std::chrono::steady_clock::now()};return true;
}
// A Matter UDP report cannot split a single string across packets. Keep
// optional previews within one report; counts still cover the complete table.
inline constexpr std::size_t kRadioPreviewLimit=768;
inline constexpr const char *kRadioPreviewTruncated="[truncated; count is complete]";
inline std::string RadioTablePreview(const std::string &table){
    if(table.size()<=kRadioPreviewLimit)return table;
    const std::string suffix=std::string("\n")+kRadioPreviewTruncated;
    const auto room=kRadioPreviewLimit-suffix.size();
    auto end=table.rfind('\n',room);
    if(end==std::string::npos)return kRadioPreviewTruncated;
    return table.substr(0,end)+suffix;
}
inline bool ParseRadioCounters(const std::vector<std::string> &lines,const std::string &uptime,RadioCounters &out){
    if(lines.size()>64)return false;std::map<std::string,uint64_t> counters;uint64_t up;
    if(!RadioUnsigned(uptime,UINT64_MAX,up))return false;
    for(auto &line:lines){auto pos=line.find(':');if(pos==line.npos)return false;auto name=RadioTrim(line.substr(0,pos));uint64_t n;
        if(name.empty()||name.size()>64||!RadioUnsigned(RadioTrim(line.substr(pos+1)),UINT32_MAX,n)||!counters.emplace(name,n).second)return false;}
    if(!counters.count("TxTotal")||!counters.count("RxTotal")||!counters.count("TxRetry"))return false;
    out={true,uint32_t(counters["TxTotal"]),uint32_t(counters["RxTotal"]),uint32_t(counters["TxRetry"]),up,std::chrono::steady_clock::now()};return true;
}
// A lower counter may be a wrap OR reset; uptime cannot disambiguate manual resets.
// Drop the interval rather than publish a false spike. No rate is exposed as routing usage.
inline std::optional<uint32_t> RadioCounterDelta(const RadioCounters &before,const RadioCounters &after,uint32_t a,uint32_t b){
    if(!before.Fresh()||!after.Fresh()||after.uptimeMs<=before.uptimeMs||b<a)return std::nullopt;return b-a;
}
}
