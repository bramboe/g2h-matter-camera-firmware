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
        std::lock_guard<std::mutex> lock(mutex_);if(started_)return true;
        if(mkdir(directory_.c_str(),0700)&&errno!=EEXIST)return false;
        struct stat st{};if(lstat(directory_.c_str(),&st)||!S_ISDIR(st.st_mode)||(st.st_mode&077))return false;
        stop_=false;
        if(!Spawn(encodeThread_,EncodeEntry))return false;
        if(!Spawn(playThread_,PlayEntry)){stop_=true;cv_.notify_all();pthread_join(encodeThread_,nullptr);return false;}
        started_=true;return true;
    }
    void Stop() noexcept {
        {std::lock_guard<std::mutex> lock(mutex_);if(!started_)return;stop_=true;}
        cv_.notify_all();pthread_join(encodeThread_,nullptr);pthread_join(playThread_,nullptr);
        std::lock_guard<std::mutex> lock(mutex_);started_=false;opus_.clear();
        for(auto &path:ready_)std::remove(path.c_str());ready_.clear();
    }
    // Called from the WebRTC thread; never blocks.
    void PushOpus(const std::uint8_t *data,std::size_t size){
        if(!data||!size||size>1275)return;
        ++received_;
        std::lock_guard<std::mutex> lock(mutex_);if(!started_||muted_)return;
        if(opus_.size()>=50)opus_.pop_front(); // at most 1 s of 20 ms packets
        opus_.emplace_back(data,data+size);cv_.notify_all();
    }
    void SetMuted(bool muted){std::lock_guard<std::mutex> lock(mutex_);muted_=muted;if(muted)opus_.clear();}
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
    void Encode() noexcept {
        int error=0;OpusDecoder *decoder=opus_decoder_create(16000,1,&error);
        auto encoder=TalkEncoder::Create();
        if(!decoder||error!=OPUS_OK||!encoder){if(decoder)opus_decoder_destroy(decoder);return;}
        std::vector<std::vector<std::uint8_t>> frames;std::vector<std::uint8_t> chunk;std::int16_t pcm[1920];
        std::unique_lock<std::mutex> lock(mutex_);
        while(!stop_){
            cv_.wait(lock,[&]{return stop_||!opus_.empty();});
            if(stop_)break;
            auto packet=std::move(opus_.front());opus_.pop_front();lock.unlock();
            const int samples=opus_decode(decoder,packet.data(),opus_int32(packet.size()),pcm,1920,0);
            frames.clear();
            if(samples>0&&encoder->Push(pcm,std::size_t(samples),frames)){
                for(auto &f:frames){
                    chunk.insert(chunk.end(),f.begin(),f.end());
                    if(++framesInChunk_==kFramesPerChunk){Publish(chunk);chunk.clear();framesInChunk_=0;}
                }
            }
            lock.lock();
        }
        opus_decoder_destroy(decoder);
    }
    void Publish(const std::vector<std::uint8_t> &chunk){
        if(!ValidAdts(chunk))return;
        const std::string path=directory_+"/talk-"+std::to_string(sequence_++%16)+".aac",part=path+".part";
        FILE *f=std::fopen(part.c_str(),"wb");if(!f)return;
        const bool ok=std::fwrite(chunk.data(),1,chunk.size(),f)==chunk.size();
        if(std::fclose(f)||!ok||std::rename(part.c_str(),path.c_str())){std::remove(part.c_str());return;}
        std::lock_guard<std::mutex> lock(mutex_);
        if(ready_.size()>=2){std::remove(ready_.front().c_str());ready_.pop_front();}
        ready_.push_back(path);++published_;cv_.notify_all();
    }
    void Play() noexcept {
        std::unique_lock<std::mutex> lock(mutex_);
        while(!stop_){
            cv_.wait(lock,[&]{return stop_||!ready_.empty();});
            if(stop_)break;
            const std::string path=ready_.front();ready_.pop_front();lock.unlock();
            std::string reply;
            // Synchronous for the chunk's length; bounded at chunk length + 2 s.
            if(VendorExchange("function debug_playfile "+path,reply,std::chrono::milliseconds(2600)))++played_;
            std::remove(path.c_str());
            lock.lock();
        }
    }
    const std::string directory_;
    std::mutex mutex_;
    std::condition_variable cv_;
    pthread_t encodeThread_{},playThread_{};
    bool started_=false,stop_=false,muted_=false;
    std::deque<std::vector<std::uint8_t>> opus_;
    std::deque<std::string> ready_;
    std::size_t framesInChunk_=0;
    std::uint64_t sequence_=0;
    std::atomic<std::uint64_t> played_{0},received_{0},published_{0};
};
}
