#pragma once
#include "aac_hbr.hpp"
#include "g2h_media_input.hpp"
#include "g2h_aac_opus.hpp"
#include <pthread.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

namespace g2h {
inline constexpr const char *kAudioPath="/13";
// One loopback RTSP client on /13, audio track only, closed gracefully,
// AAC-LC 16 kHz mono transcoded to Opus 48 kHz 20 ms. Opened on first use and
// kept open until the app exits: closing vendor RTSP sessions has hung the
// vendor server, so a failed ingest is reported, not reconnected.
class AudioSource {
public:
    struct Packet {std::vector<std::uint8_t> opus;std::uint32_t timestamp=0;};
    AudioSource()=default;
    ~AudioSource(){Stop();}
    AudioSource(const AudioSource&)=delete;
    AudioSource& operator=(const AudioSource&)=delete;
    // Non-blocking: the RTSP handshake runs on the worker.
    bool Start(unsigned bitRate){
        bitRate_=bitRate;
        std::lock_guard<std::mutex> lock(mutex_);
        if(started_)return !fault_;
        stop_=false;fault_=false;pthread_attr_t attr;
        if(pthread_attr_init(&attr))return false;
        const bool ok=!pthread_attr_setstacksize(&attr,1024*1024)&&!pthread_create(&thread_,&attr,Entry,this);
        pthread_attr_destroy(&attr);started_=ok;return ok;
    }
    void Stop() noexcept {
        {std::lock_guard<std::mutex> lock(mutex_);if(!started_)return;}
        stop_=true;pthread_join(thread_,nullptr);
        std::lock_guard<std::mutex> lock(mutex_);started_=false;queue_.clear();
    }
    void EnableFrames(bool enable){
        std::lock_guard<std::mutex> lock(mutex_);
        if(streaming_.exchange(enable)!=enable)queue_.clear();
    }
    void SetBitrate(unsigned bitRate){bitRate_=bitRate;}
    bool Pop(Packet &packet){
        std::lock_guard<std::mutex> lock(mutex_);
        if(!streaming_||queue_.empty())return false;
        packet=std::move(queue_.front());queue_.pop_front();return true;
    }
    bool Faulted() const {return fault_;}
    bool Running() const {return running_;}
    // Last step reached; on a fault, the step that failed. Static strings only.
    const char *Stage() const {return stage_;}
    // Diagnostics: RTP packets on the audio channel, AAC access units, transcoder errors.
    std::uint64_t RtpPackets() const {return rtpPackets_;}
    std::uint64_t AccessUnits() const {return accessUnits_;}
    std::uint64_t DecodeErrors() const {return decodeErrors_;}
    unsigned Channel() const {return channel_;}
    std::uint64_t AdtsStripped() const {return adtsStripped_;}
    unsigned LastError() const {return lastError_;} // g2hmedia::AacToOpus::Error value
private:
    static constexpr std::size_t kMaxQueued=25; // 0.5 s of 20 ms packets
    static void *Entry(void *self){static_cast<AudioSource*>(self)->Run();return nullptr;}
    void Run() noexcept {
        try{
            // Graceful teardown: drain pending RTP and the TEARDOWN reply before close. A plain
            // close with unread data resets the connection, and every stop of our app after a
            // /13 session restarted the vendor camera process (2026-10-22/23).
            stage_="connect";MediaInput source(true);
            stage_="describe-setup-play";source.Start();const auto track=source.AudioTrack();const unsigned payload=track.payload;channel_=track.channel;
            stage_="transcoder";
            static constexpr std::uint8_t kAsc[2]={0x14,0x08}; // AAC-LC, 16 kHz, mono (SDP config=1408)
            g2hmedia::AacToOpus::Error error;
            auto transcoder=g2hmedia::AacToOpus::Create(kAsc,2,16000,1,error);
            if(!transcoder)throw std::runtime_error("audio transcoder");
            AacHbrDepacketizer depacketizer(payload);
            unsigned appliedRate=0;std::vector<std::uint8_t> packet;g2hmedia::AacToOpus::Batch batch;
            running_=true;stage_="reading";
            while(!stop_){
                source.RenewReadDeadline();std::uint8_t header[4];source.Read(header,4);
                if(header[0]!='$')throw std::runtime_error("RTSP record");
                packet.resize((header[2]<<8)|header[3]);if(packet.empty())throw std::runtime_error("empty RTP");
                source.Read(packet.data(),packet.size());if(header[1]!=channel_)continue;
                ++rtpPackets_;
                const unsigned rate=bitRate_;
                if(rate!=appliedRate&&transcoder->SetBitrate(int(rate)))appliedRate=rate;
                (void)depacketizer.Push(packet.data(),packet.size(),[&](const std::uint8_t *au,std::size_t size){
                    // A bad AU resets the transcoder and is skipped; the ingest continues.
                    ++accessUnits_;
                    // The vendor may carry whole ADTS frames inside AAC-hbr; strip a header
                    // only when it matches this track exactly (LC, 16 kHz, mono, length == AU).
                    if(size>9&&au[0]==0xff&&(au[1]&0xf6)==0xf0&&((au[2]>>6)&3)==1&&((au[2]>>2)&15)==8&&
                       (((au[2]&1)<<2)|(au[3]>>6))==1&&((std::size_t(au[3]&3)<<11)|(std::size_t(au[4])<<3)|(au[5]>>5))==size){
                        const std::size_t header=(au[1]&1)?7:9;au+=header;size-=header;++adtsStripped_;
                    }
                    const auto error=transcoder->Push(au,size,batch);
                    if(error!=g2hmedia::AacToOpus::Error::None){++decodeErrors_;lastError_=unsigned(error);return;}
                    std::lock_guard<std::mutex> lock(mutex_);
                    if(!streaming_)return;
                    for(std::size_t i=0;i<batch.count;++i){
                        if(queue_.size()>=kMaxQueued)queue_.pop_front();
                        const auto &opus=batch.packets[i];
                        queue_.push_back(Packet{std::vector<std::uint8_t>(opus.bytes.begin(),opus.bytes.begin()+opus.size),opus.timestamp});
                    }
                });
            }
        }catch(...){if(!stop_)fault_=true;} // stage_ keeps the failing step
        running_=false;
    }
    std::mutex mutex_;
    pthread_t thread_{};
    bool started_=false;
    std::atomic<bool> stop_{false},streaming_{false},fault_{false},running_{false};
    std::atomic<unsigned> bitRate_{32000};
    std::atomic<const char*> stage_{"idle"};
    std::atomic<unsigned> channel_{0};
    std::atomic<std::uint64_t> rtpPackets_{0},accessUnits_{0},decodeErrors_{0},adtsStripped_{0};
    std::atomic<unsigned> lastError_{0};
    std::deque<Packet> queue_;
};
}
