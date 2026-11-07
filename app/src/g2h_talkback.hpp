#pragma once
#include "g2h_adts.hpp"
#include "g2h_vendor_control.hpp"
#include <pthread.h>
#include <sys/stat.h>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#ifdef G2H_TALKBACK_TEST_HOOKS
#include <functional>
#endif
#include <memory>
#include <mutex>
#include <string>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <opus/opus.h>
}

namespace g2h {
// Talkback through the vendor's own playback ("function debug_playfile PATH",
// verified audible 2026-10-22). That reader takes AAC ADTS frames and copies each
// frame into a 1024-byte stack buffer without a size check, so every frame we
// write is bounded well below it. Speaker ownership stays with the vendor process.
inline constexpr std::size_t kFramesPerChunk=8; // 8×1024 samples = 0.512 s at 16 kHz

// PCM 16 kHz mono → AAC-LC ADTS frames.
class TalkEncoder {
public:
    static std::unique_ptr<TalkEncoder> Create(int bitRate=32000){
        std::unique_ptr<TalkEncoder> e(new TalkEncoder);return e->Init(bitRate)?std::move(e):nullptr;
    }
    ~TalkEncoder(){av_packet_free(&packet_);av_frame_free(&frame_);avcodec_free_context(&ctx_);}
    TalkEncoder(const TalkEncoder&)=delete;
    TalkEncoder& operator=(const TalkEncoder&)=delete;
    // Appends complete ADTS frames to out; false on an encoder error or an oversized frame.
    bool Push(const std::int16_t *pcm,std::size_t count,std::vector<std::vector<std::uint8_t>> &out){
        for(std::size_t i=0;i<count;++i){
            pending_.push_back(float(pcm[i])/32768.0f);
            if(pending_.size()==1024&&!Encode(out))return false;
        }
        return true;
    }
private:
    TalkEncoder()=default;
    bool Init(int bitRate){
        const AVCodec *codec=avcodec_find_encoder(AV_CODEC_ID_AAC);if(!codec)return false;
        ctx_=avcodec_alloc_context3(codec);frame_=av_frame_alloc();packet_=av_packet_alloc();
        if(!ctx_||!frame_||!packet_)return false;
        ctx_->sample_rate=16000;ctx_->sample_fmt=AV_SAMPLE_FMT_FLTP;ctx_->bit_rate=bitRate;ctx_->profile=AV_PROFILE_AAC_LOW;
        ctx_->thread_count=1;av_channel_layout_default(&ctx_->ch_layout,1);
        if(avcodec_open2(ctx_,codec,nullptr)<0||ctx_->frame_size!=1024)return false;
        frame_->nb_samples=1024;frame_->format=AV_SAMPLE_FMT_FLTP;frame_->sample_rate=16000;
        if(av_channel_layout_copy(&frame_->ch_layout,&ctx_->ch_layout)<0||av_frame_get_buffer(frame_,0)<0)return false;
        pending_.reserve(1024);return true;
    }
    bool Encode(std::vector<std::vector<std::uint8_t>> &out){
        if(av_frame_make_writable(frame_)<0)return false;
        std::copy(pending_.begin(),pending_.end(),reinterpret_cast<float*>(frame_->data[0]));pending_.clear();
        frame_->pts=pts_;pts_+=1024;
        if(avcodec_send_frame(ctx_,frame_)<0)return false;
        for(;;){
            const int r=avcodec_receive_packet(ctx_,packet_);
            if(r==AVERROR(EAGAIN))return true;
            if(r<0)return false;
            const std::size_t total=std::size_t(packet_->size)+7;
            if(packet_->size<=0||total>kMaxAdtsFrame){av_packet_unref(packet_);return false;}
            // ADTS header: MPEG-4, no CRC, AAC LC, 16 kHz (index 8), mono, one raw block.
            std::vector<std::uint8_t> f(total);
            f[0]=0xff;f[1]=0xf1;f[2]=std::uint8_t((1<<6)|(8<<2)|0);f[3]=std::uint8_t((1<<6)|(total>>11));
            f[4]=std::uint8_t((total>>3)&0xff);f[5]=std::uint8_t(((total&7)<<5)|0x1f);f[6]=0xfc;
            std::copy(packet_->data,packet_->data+packet_->size,f.begin()+7);av_packet_unref(packet_);
            out.push_back(std::move(f));
        }
    }
    AVCodecContext *ctx_=nullptr;AVFrame *frame_=nullptr;AVPacket *packet_=nullptr;
    std::vector<float> pending_;std::int64_t pts_=0;
};

// Opus from the controller → 0.5 s AAC chunks in a private directory → vendor playback.
// Two workers: encoding never waits for playback. Each queue is bounded; old audio
// is dropped rather than delayed. One talk session at a time.
class Talkback {
public:
    explicit Talkback(std::string directory):directory_(std::move(directory)){}
    ~Talkback(){Stop();}
    Talkback(const Talkback&)=delete;
    Talkback& operator=(const Talkback&)=delete;
    bool Start(){
        std::unique_lock<std::mutex> lock(mutex_);if(started_)return true;
        if(mkdir(directory_.c_str(),0700)&&errno!=EEXIST)return false;
        struct stat st{};if(lstat(directory_.c_str(),&st)||!S_ISDIR(st.st_mode)||(st.st_mode&077))return false;
        stop_=false;
        if(!Spawn(encodeThread_,EncodeEntry))return false;
        if(!Spawn(playThread_,PlayEntry)){stop_=true;++generation_;cv_.notify_all();lock.unlock();pthread_join(encodeThread_,nullptr);return false;}
        started_=true;return true;
    }
    void Stop() noexcept {
        {std::lock_guard<std::mutex> lock(mutex_);if(!started_)return;stop_=true;++generation_;opus_.clear();ClearReady();}
        cv_.notify_all();pthread_join(encodeThread_,nullptr);pthread_join(playThread_,nullptr);
        std::lock_guard<std::mutex> lock(mutex_);started_=false;opus_.clear();
        ClearReady();
    }
    // Called from the WebRTC thread; takes only the bounded queue mutex.
    void PushOpus(const std::uint8_t *data,std::size_t size){
        if(!data||!size||size>1275)return;
        ++received_;
        std::lock_guard<std::mutex> lock(mutex_);if(!started_||stop_||muted_)return;
        if(opus_.size()>=50)opus_.pop_front(); // at most 1 s of 20 ms packets
        opus_.emplace_back(data,data+size);cv_.notify_all();
    }
    void SetMuted(bool muted){
        std::lock_guard<std::mutex> lock(mutex_);
        if(muted_==muted)return;
        muted_=muted;++generation_;opus_.clear();ClearReady();cv_.notify_all();
    }
#ifdef G2H_TALKBACK_TEST_HOOKS
    // Deterministic worker barriers; absent from production builds.
    std::function<void()> beforeEncode,afterEncode,beforePublishCommit,beforePlaybackDispatch;
#endif
    std::uint64_t PlayedChunks() const {return played_;}
    std::uint64_t ReceivedPackets() const {return received_;}
    std::uint64_t PublishedChunks() const {return published_;}
private:
    bool Spawn(pthread_t &thread,void*(*entry)(void*)){
        pthread_attr_t attr;if(pthread_attr_init(&attr))return false;
        const bool ok=!pthread_attr_setstacksize(&attr,512*1024)&&!pthread_create(&thread,&attr,entry,this);
        pthread_attr_destroy(&attr);return ok;
    }
    static void *EncodeEntry(void *self){static_cast<Talkback*>(self)->Encode();return nullptr;}
    static void *PlayEntry(void *self){static_cast<Talkback*>(self)->Play();return nullptr;}
    void ClearReady(){for(auto &item:ready_)std::remove(item.path.c_str());ready_.clear();}
    bool Current(std::uint64_t generation) const {return !stop_&&!muted_&&generation==generation_;}
    void Encode() noexcept {
        int error=0;OpusDecoder *decoder=opus_decoder_create(16000,1,&error);
        auto encoder=TalkEncoder::Create();
        if(!decoder||error!=OPUS_OK||!encoder){if(decoder)opus_decoder_destroy(decoder);return;}
        std::vector<std::vector<std::uint8_t>> frames;std::vector<std::uint8_t> chunk;std::int16_t pcm[1920];
        std::size_t framesInChunk=0;std::uint64_t codecGeneration=UINT64_MAX;
        std::unique_lock<std::mutex> lock(mutex_);
        while(!stop_){
            cv_.wait(lock,[&]{return stop_||codecGeneration!=generation_||!opus_.empty();});
            if(stop_)break;
            if(codecGeneration!=generation_){
                codecGeneration=generation_;lock.unlock();
                // Discard delayed AAC packets, partial PCM and partial chunks even
                // when no fresh packets arrive after a privacy/mute transition.
                encoder=TalkEncoder::Create();opus_decoder_ctl(decoder,OPUS_RESET_STATE);
                frames.clear();chunk.clear();framesInChunk=0;
                lock.lock();continue;
            }
            auto packet=std::move(opus_.front());opus_.pop_front();
            const auto generation=generation_;lock.unlock();
#ifdef G2H_TALKBACK_TEST_HOOKS
            if(beforeEncode)beforeEncode();
#endif
            if(!encoder){lock.lock();continue;}
            const int samples=opus_decode(decoder,packet.data(),opus_int32(packet.size()),pcm,1920,0);
            frames.clear();
            if(samples>0&&encoder->Push(pcm,std::size_t(samples),frames)){
                for(auto &f:frames){
                    chunk.insert(chunk.end(),f.begin(),f.end());
                    if(++framesInChunk==kFramesPerChunk){Publish(chunk,generation);chunk.clear();framesInChunk=0;}
                }
            }
#ifdef G2H_TALKBACK_TEST_HOOKS
            if(afterEncode)afterEncode();
#endif
            lock.lock();
        }
        opus_decoder_destroy(decoder);
    }
    void Publish(const std::vector<std::uint8_t> &chunk,std::uint64_t generation){
        if(!ValidAdts(chunk))return;
        const std::string path=directory_+"/talk-"+std::to_string(sequence_++)+".aac",part=path+".part";
        FILE *f=std::fopen(part.c_str(),"wb");if(!f)return;
        const bool ok=std::fwrite(chunk.data(),1,chunk.size(),f)==chunk.size();
        if(std::fclose(f)||!ok){std::remove(part.c_str());return;}
#ifdef G2H_TALKBACK_TEST_HOOKS
        if(beforePublishCommit)beforePublishCommit();
#endif
        std::lock_guard<std::mutex> lock(mutex_);
        if(!Current(generation)||std::rename(part.c_str(),path.c_str())){std::remove(part.c_str());return;}
        if(ready_.size()>=2){std::remove(ready_.front().path.c_str());ready_.pop_front();}
        ready_.push_back({path,generation});++published_;cv_.notify_all();
    }
    void Play() noexcept {
        std::unique_lock<std::mutex> lock(mutex_);
        while(!stop_){
            cv_.wait(lock,[&]{return stop_||!ready_.empty();});
            if(stop_)break;
            auto item=std::move(ready_.front());ready_.pop_front();lock.unlock();
#ifdef G2H_TALKBACK_TEST_HOOKS
            if(beforePlaybackDispatch)beforePlaybackDispatch();
#endif
            // Synchronous for the chunk's length; bounded at chunk length + 2 s.
            if(PlayFile(item.path,item.generation))++played_;
            std::remove(item.path.c_str());
            lock.lock();
        }
    }
    // Connect/read are outside the queue lock. Only the bounded nonblocking send
    // is serialized with SetMuted, so a stale command cannot be sent afterward.
    // Audio already accepted by the vendor before mute may finish its 0.512 s chunk.
    bool PlayFile(const std::string &path,std::uint64_t generation){
        const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(2600);
        auto wait=[&](int fd,short events){
            for(;;){
                {std::lock_guard<std::mutex> lock(mutex_);if(!Current(generation))return false;}
                const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(until-std::chrono::steady_clock::now()).count();
                if(remaining<=0)return false;
                pollfd p{fd,events,0};const int rc=poll(&p,1,int(std::min<std::int64_t>(remaining,50)));
                if(rc>0)return (p.revents&(events|POLLHUP))!=0;
                if(rc<0&&errno!=EINTR)return false;
            }
        };
        const int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return false;
        struct Close {int fd;~Close(){close(fd);}} closer{fd};
        if(fcntl(fd,F_SETFD,FD_CLOEXEC)<0||fcntl(fd,F_SETFL,O_NONBLOCK)<0)return false;
#ifdef MSG_NOSIGNAL
        constexpr int sendFlags=MSG_NOSIGNAL;
#else
        constexpr int sendFlags=0;int one=1;if(setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof one)<0)return false;
#endif
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(vendorDebugPort);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof address)<0&&errno!=EINPROGRESS)return false;
        if(!wait(fd,POLLOUT))return false;
        int error=0;socklen_t length=sizeof error;
        if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&length)<0||error)return false;
        const std::string command="function debug_playfile "+path+'\0';
        // Send a whole bounded command once; never retry a partial command or
        // dispatch a path from a previous mute generation.
        {
            std::lock_guard<std::mutex> lock(mutex_);if(!Current(generation))return false;
            if(send(fd,command.data(),command.size(),sendFlags)!=ssize_t(command.size()))return false;
            if(shutdown(fd,SHUT_WR))return false;
        }
        std::size_t bytes=0;char buffer[128];
        for(;;){
            if(!wait(fd,POLLIN))return false;
            const auto n=recv(fd,buffer,sizeof buffer,0);
            if(n==0)return true;
            if(n<0){if(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK)continue;return false;}
            bytes+=std::size_t(n);if(bytes>256)return false;
        }
    }
    const std::string directory_;
    std::mutex mutex_;
    std::condition_variable cv_;
    pthread_t encodeThread_{},playThread_{};
    bool started_=false,stop_=false,muted_=false;
    std::deque<std::vector<std::uint8_t>> opus_;
    struct ReadyChunk {std::string path;std::uint64_t generation;};
    std::deque<ReadyChunk> ready_;
    std::uint64_t generation_=0;
    std::uint64_t sequence_=0;
    std::atomic<std::uint64_t> played_{0},received_{0},published_{0};
};
}
