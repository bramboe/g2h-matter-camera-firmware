#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <cerrno>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include <opus/opus.h>
}
namespace g2hmedia {
// Deliberately restricted to the observed G2H AAC-LC/16000/mono source.
// A caller must supply and verify actual source ASC; this code does not infer it.
class AacToOpus final {
public:
    static constexpr size_t kMaxAu=2048, kOpusSamples=960, kMaxPacket=1275, kMaxPacketsPerAu=4;
    struct Packet { std::array<uint8_t,kMaxPacket> bytes{}; size_t size=0; uint32_t timestamp=0; };
    struct Batch { std::array<Packet,kMaxPacketsPerAu> packets{}; size_t count=0; };
    struct Stats { uint64_t aacFrames=0, decodedSourceSamples=0, opusPackets=0; };
    enum class Error { None, Config, Allocation, Decoder, Resampler, Encoder, AuSize, Decode, FrameFormat, QueueLimit };
    static bool ValidConfig(const uint8_t *asc,size_t len,unsigned rate,unsigned channels) noexcept {
        // AOT=2, indexed sample rate=16000 (8), channelConfiguration=1,
        // GASpecific frameLengthFlag/dependsOnCoreCoder/extensionFlag all zero.
        return asc && len==2 && asc[0]==0x14 && asc[1]==0x08 && rate==16000 && channels==1;
    }
    static std::unique_ptr<AacToOpus> Create(const uint8_t *asc,size_t len,unsigned rate,unsigned channels,Error &error) {
        error=Error::Config;
        if(!ValidConfig(asc,len,rate,channels)) return {};
        std::unique_ptr<AacToOpus> p(new(std::nothrow) AacToOpus);
        if(!p){error=Error::Allocation;return {};}
        if(!p->Init(asc,error))return {};
        error=Error::None;return p;
    }
    ~AacToOpus() {
        if(enc_)opus_encoder_destroy(enc_);
        swr_free(&swr_);av_packet_free(&packet_);av_frame_free(&frame_);avcodec_free_context(&decoder_);
        volatile int16_t *q=pcm_.data();for(size_t i=0;i<pcm_.size();++i)q[i]=0;
        volatile int16_t *r=resampled_.data();for(size_t i=0;i<resampled_.size();++i)r[i]=0;
    }
    AacToOpus(const AacToOpus&)=delete;AacToOpus&operator=(const AacToOpus&)=delete;
    Stats GetStats()const noexcept{return stats_;}
    // Opus accepts 500..512000 b/s; callers pass the Matter-allocated bit rate.
    bool SetBitrate(int bitsPerSecond) noexcept {return bitsPerSecond>=500&&bitsPerSecond<=512000&&opus_encoder_ctl(enc_,OPUS_SET_BITRATE(bitsPerSecond))==OPUS_OK;}
    size_t QueuedSamples()const noexcept{return queued_;}
    // Each call consumes exactly one complete, de-fragmented raw AAC AU. No ADTS,
    // LATM, RTP headers, multi-AU aggregate or arbitrary byte stream is accepted.
    // Output is encoded Opus only; timestamps are a monotonic 48kHz media clock.
    Error Push(const uint8_t *au,size_t size,Batch &out) noexcept {
        out.count=0;
        if(!au||size==0||size>kMaxAu)return Error::AuSize;
        if(size>=2&&au[0]==0xff&&(au[1]&0xf6)==0xf0)return Error::Decode;
        av_packet_unref(packet_);
        if(av_new_packet(packet_,static_cast<int>(size))<0)return Error::Allocation;
        std::memcpy(packet_->data,au,size); // av_new_packet supplies zero input padding.
        int sent=avcodec_send_packet(decoder_,packet_);av_packet_unref(packet_);
        if(sent<0)return Fail(Error::Decode,out);
        av_frame_unref(frame_);
        if(avcodec_receive_frame(decoder_,frame_)<0)return Fail(Error::Decode,out);
        if(frame_->sample_rate!=16000||frame_->ch_layout.nb_channels!=1||frame_->nb_samples!=1024||frame_->format!=AV_SAMPLE_FMT_FLTP||!frame_->extended_data||!frame_->extended_data[0])return Fail(Error::FrameFormat,out);
        const uint8_t *input[1]={frame_->extended_data[0]};uint8_t *output[1]={reinterpret_cast<uint8_t*>(resampled_.data())};
        int n=swr_convert(swr_,output,static_cast<int>(resampled_.size()),input,frame_->nb_samples);
        if(n<0||static_cast<size_t>(n)>resampled_.size())return Fail(Error::Resampler,out);
        if(queued_+static_cast<size_t>(n)>pcm_.size())return Fail(Error::QueueLimit,out);
        std::memcpy(pcm_.data()+queued_,resampled_.data(),static_cast<size_t>(n)*sizeof(int16_t));queued_+=static_cast<size_t>(n);
        ++stats_.aacFrames;stats_.decodedSourceSamples+=1024;
        // One AAC AU must not silently produce additional queued decoder frames.
        av_frame_unref(frame_);int extra=avcodec_receive_frame(decoder_,frame_);av_frame_unref(frame_);
        if(extra!=AVERROR(EAGAIN))return Fail(Error::Decode,out);
        while(queued_>=kOpusSamples){
            if(out.count==out.packets.size())return Fail(Error::QueueLimit,out);
            auto &p=out.packets[out.count];int bytes=opus_encode(enc_,pcm_.data(),kOpusSamples,p.bytes.data(),p.bytes.size());
            if(bytes<=0||bytes>static_cast<int>(kMaxPacket))return Fail(Error::Encoder,out);
            p.size=static_cast<size_t>(bytes);p.timestamp=nextTimestamp_;nextTimestamp_+=kOpusSamples;
            ++out.count;++stats_.opusPackets;queued_-=kOpusSamples;
            std::memmove(pcm_.data(),pcm_.data()+kOpusSamples,queued_*sizeof(int16_t));
        }
        return Error::None;
    }
private:
    AacToOpus()=default;
    bool Init(const uint8_t *asc,Error &err) noexcept {
        const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_AAC);if(!codec){err=Error::Decoder;return false;}
        decoder_=avcodec_alloc_context3(codec);packet_=av_packet_alloc();frame_=av_frame_alloc();
        if(!decoder_||!packet_||!frame_){err=Error::Allocation;return false;}
        decoder_->extradata=static_cast<uint8_t*>(av_mallocz(2+AV_INPUT_BUFFER_PADDING_SIZE));
        if(!decoder_->extradata){err=Error::Allocation;return false;}
        std::memcpy(decoder_->extradata,asc,2);decoder_->extradata_size=2;decoder_->sample_rate=16000;
        av_channel_layout_default(&decoder_->ch_layout,1);decoder_->thread_count=1;decoder_->max_samples=2048; // FFmpeg AAC reserves 2048 internally; output below must be 1024.
        decoder_->err_recognition=AV_EF_CAREFUL|AV_EF_EXPLODE;
        if(avcodec_open2(decoder_,codec,nullptr)<0){err=Error::Decoder;return false;}
        AVChannelLayout mono=AV_CHANNEL_LAYOUT_MONO;
        if(swr_alloc_set_opts2(&swr_,&mono,AV_SAMPLE_FMT_S16,48000,&mono,AV_SAMPLE_FMT_FLTP,16000,0,nullptr)<0||!swr_||swr_init(swr_)<0){err=Error::Resampler;return false;}
        int oe=0;enc_=opus_encoder_create(48000,1,OPUS_APPLICATION_VOIP,&oe);
        if(!enc_||oe!=OPUS_OK){err=Error::Encoder;return false;}
        if(opus_encoder_ctl(enc_,OPUS_SET_BITRATE(24000))!=OPUS_OK||opus_encoder_ctl(enc_,OPUS_SET_COMPLEXITY(3))!=OPUS_OK||opus_encoder_ctl(enc_,OPUS_SET_VBR(1))!=OPUS_OK||opus_encoder_ctl(enc_,OPUS_SET_DTX(0))!=OPUS_OK||opus_encoder_ctl(enc_,OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE))!=OPUS_OK){err=Error::Encoder;return false;}
        return true;
    }
    Error Fail(Error e,Batch &out) noexcept {
        out.count=0;queued_=0;avcodec_flush_buffers(decoder_);av_frame_unref(frame_);
        swr_close(swr_);if(swr_init(swr_)<0)return Error::Resampler;
        opus_encoder_ctl(enc_,OPUS_RESET_STATE);return e;
    }
    AVCodecContext *decoder_=nullptr;AVPacket *packet_=nullptr;AVFrame *frame_=nullptr;
    SwrContext *swr_=nullptr;OpusEncoder *enc_=nullptr;
    std::array<int16_t,7680> pcm_{};std::array<int16_t,4096> resampled_{};
    size_t queued_=0;uint32_t nextTimestamp_=0;Stats stats_{};
};
} // namespace g2hmedia
