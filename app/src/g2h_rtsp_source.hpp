// Bounded RTSP source shared by the live-ingest proof and Linux camera app.
#pragma once
#include "h264_rtp.hpp"
#include "g2h_video_profile.hpp"
#include "h264_sdp.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <cctype>

using Clock=std::chrono::steady_clock;
class Rtsp {
    int fd_=-1;
    unsigned seq_=0;
    std::string uri_,session_,lastHeaders_;
    bool gracefulTeardown_=false;
    Clock::time_point deadline_=Clock::now()+std::chrono::seconds(9);
    void Wait(short what){
        for(;;){
            auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline_-Clock::now()).count();
            if(left<=0)throw std::runtime_error("RTSP deadline");
            pollfd p{fd_,what,0};int rc=poll(&p,1,static_cast<int>(left));
            if(rc<0&&errno==EINTR)continue;
            if(rc<=0||!(p.revents&what))throw std::runtime_error("RTSP transport unavailable");
            return;
        }
    }
    void Send(const std::string &s){
        std::size_t at=0;
        while(at<s.size()){
            Wait(POLLOUT);
            auto n=send(fd_,s.data()+at,s.size()-at,
#ifdef MSG_NOSIGNAL
                MSG_NOSIGNAL
#else
                0
#endif
            );
            if(n<0&&(errno==EAGAIN||errno==EINTR))continue;
            if(n<=0)throw std::runtime_error("RTSP send");
            at+=static_cast<std::size_t>(n);
        }
    }
    std::string Request(const char*method,const std::string &suffix,const std::string &extra){
        std::string q=std::string(method)+" "+uri_+suffix+" RTSP/1.0\r\nCSeq: "+std::to_string(++seq_)+"\r\nUser-Agent: G2H-Matter-ingest/0.1\r\n";
        if(!session_.empty())q+="Session: "+session_+"\r\n";
        Send(q+extra+"\r\n");std::string h;
        while(h.size()<8192){char c;Read(&c,1);h+=c;if(h.size()>=4&&h.compare(h.size()-4,4,"\r\n\r\n")==0)break;}
        if(h.size()>=8192||h.rfind("RTSP/1.0 200 ",0)!=0)throw std::runtime_error("RTSP rejected request");
        lastHeaders_=h;
        auto session=h.find("\r\nSession: ");if(session!=std::string::npos){session+=11;auto end=h.find_first_of(";\r\n",session);if(end==std::string::npos||end-session>64)throw std::runtime_error("invalid RTSP session");session_=h.substr(session,end-session);}
        auto lower=h;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        std::size_t length=0;auto p=lower.find("\r\ncontent-length:");
        if(p!=std::string::npos){p+=17;while(p<h.size()&&h[p]==' ')++p;std::size_t start=p;for(;p<h.size()&&h[p]>='0'&&h[p]<='9';++p){length=length*10+h[p]-'0';if(length>8192)throw std::runtime_error("oversized RTSP body");}if(p==start)throw std::runtime_error("invalid RTSP length");}
        std::string body(length,'\0');if(length)Read(body.data(),length);return body;
    }
public:
    g2h::H264Parameters parameters;
    explicit Rtsp(const char*host,std::chrono::milliseconds budget=std::chrono::milliseconds(9000),bool gracefulTeardown=false,const char *path=g2h::kVideoPath,std::uint16_t port=554):uri_(std::string("rtsp://")+host+":"+std::to_string(port)+path),gracefulTeardown_(gracefulTeardown){
        if(budget.count()<50||budget.count()>9000)throw std::invalid_argument("RTSP budget");
        deadline_=Clock::now()+budget;
        fd_=socket(AF_INET,SOCK_STREAM,0);if(fd_<0)throw std::runtime_error("socket");
        if(fcntl(fd_,F_SETFL,O_NONBLOCK)<0){close(fd_);fd_=-1;throw std::runtime_error("nonblocking socket");}
#ifdef SO_NOSIGPIPE
        int one=1;setsockopt(fd_,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof one);
#endif
        sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);inet_pton(AF_INET,host,&a.sin_addr);
        try{
            if(connect(fd_,reinterpret_cast<sockaddr*>(&a),sizeof a)<0&&errno!=EINPROGRESS)throw std::runtime_error("connect");
            Wait(POLLOUT);int error=0;socklen_t size=sizeof error;if(getsockopt(fd_,SOL_SOCKET,SO_ERROR,&error,&size)||error)throw std::runtime_error("connect failed");
        }catch(...){close(fd_);fd_=-1;throw;}
    }
    ~Rtsp(){Stop();}
    void Stop(){if(fd_>=0){if(!session_.empty()){auto s="TEARDOWN "+uri_+" RTSP/1.0\r\nCSeq: "+std::to_string(++seq_)+"\r\nSession: "+session_+"\r\n\r\n";
#ifdef MSG_NOSIGNAL
            send(fd_,s.data(),s.size(),MSG_NOSIGNAL);
#else
            send(fd_,s.data(),s.size(),0);
#endif
        }
        if(gracefulTeardown_){
            // Short-lived snapshot clients consume the pending RTP/TEARDOWN
            // response before close, rather than resetting unread TCP data.
            shutdown(fd_,SHUT_WR);const auto until=Clock::now()+std::chrono::milliseconds(300);
            std::uint8_t discarded[4096];std::size_t total=0;
            while(total<256*1024&&Clock::now()<until){
                auto left=std::chrono::duration_cast<std::chrono::milliseconds>(until-Clock::now()).count();
                if(left<=0)break;pollfd p{fd_,POLLIN,0};int ready=poll(&p,1,static_cast<int>(left));
                if(ready<0&&errno==EINTR)continue;
                if(ready<=0||!(p.revents&(POLLIN|POLLHUP)))break;
                auto n=recv(fd_,discarded,sizeof discarded,0);
                if(n<0&&(errno==EAGAIN||errno==EINTR))continue;
                if(n<=0)break;total+=static_cast<std::size_t>(n);
            }
        }
        close(fd_);fd_=-1;}}
    int Descriptor()const{return fd_;}
    void RenewReadDeadline(){deadline_=Clock::now()+std::chrono::seconds(2);}
    void Read(void *data,std::size_t count){
        auto*p=static_cast<std::uint8_t*>(data);
        while(count){Wait(POLLIN);auto n=recv(fd_,p,count,0);if(n<0&&(errno==EAGAIN||errno==EINTR))continue;if(n<=0)throw std::runtime_error("RTSP receive");p+=n;count-=static_cast<std::size_t>(n);}
    }
    void Start(){
        auto sdp=Request("DESCRIBE","","Accept: application/sdp\r\n");
        if(!parameters.Parse(sdp))throw std::runtime_error("valid H264 parameters absent");
        Request("SETUP","/trackID=0","Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n");
        if(session_.empty())throw std::runtime_error("RTSP session absent");
        Request("PLAY","","Range: npt=0.000-\r\n");
    }
    struct AudioTrack {unsigned payload,channel;};
    // Sets up the /13 session for its audio track. Accepts exactly the observed
    // AAC-LC 16 kHz mono AAC-hbr track. The interleaved RTP channel of the audio is
    // taken from the server's SETUP reply, not assumed from the request.
    AudioTrack StartAudio(){
        auto sdp=Request("DESCRIBE","","Accept: application/sdp\r\n");
        std::transform(sdp.begin(),sdp.end(),sdp.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        auto media=sdp.find("m=audio 0 rtp/avp ");if(media==std::string::npos)throw std::runtime_error("audio track absent");
        unsigned payload=0;std::size_t p=media+18;
        for(;p<sdp.size()&&sdp[p]>='0'&&sdp[p]<='9'&&payload<1000;++p)payload=payload*10+unsigned(sdp[p]-'0');
        if(p==media+18||payload<96||payload>127)throw std::runtime_error("audio payload type");
        const auto section=sdp.substr(media);const auto pt=std::to_string(payload);
        const std::string requiredLines[]={"a=control:trackid=1","a=rtpmap:"+pt+" mpeg4-generic/16000/1","mode=aac-hbr","sizelength=13","indexlength=3","indexdeltalength=3","config=1408"};
        for(const auto &required:requiredLines)
            if(section.find(required)==std::string::npos)throw std::runtime_error("unsupported audio format");
        // Audio track only: little pending data, so the graceful teardown can drain to the
        // TEARDOWN reply. (Setting up both tracks did not stop the vendor restarts on close:
        // 1080p RTP piled up beyond what the teardown drains.)
        Request("SETUP","/trackID=1","Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n");
        if(session_.empty())throw std::runtime_error("RTSP session absent");
        unsigned channel=2; // requested for the audio track; the reply below is authoritative
        auto at=lastHeaders_.find("interleaved=");
        if(at!=std::string::npos){
            at+=12;std::size_t digits=0;channel=0;
            while(at<lastHeaders_.size()&&std::isdigit(static_cast<unsigned char>(lastHeaders_[at]))&&digits<3){channel=channel*10+unsigned(lastHeaders_[at]-'0');++at;++digits;}
            if(!digits||channel>254)throw std::runtime_error("RTSP interleaved channel");
        }
        Request("PLAY","","Range: npt=0.000-\r\n");
        return {payload,channel};
    }
};
