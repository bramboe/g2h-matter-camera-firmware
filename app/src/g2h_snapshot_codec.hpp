#pragma once
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
namespace g2h {
// Decode one complete IDR access unit in RAM, scale, and encode a bounded JPEG.
// No files, device access, RTSP, global encoder settings, or persistent state.
inline std::vector<std::uint8_t> SnapshotJpeg(const std::vector<std::uint8_t>& idr,unsigned quality){
    if(idr.empty()||idr.size()>1024*1024||quality<1||quality>100)throw std::invalid_argument("snapshot input bounds");
    struct ContextFree{void operator()(AVCodecContext*p)const{avcodec_free_context(&p);}};
    struct FrameFree{void operator()(AVFrame*p)const{av_frame_free(&p);}};
    struct PacketFree{void operator()(AVPacket*p)const{av_packet_free(&p);}};
    using Context=std::unique_ptr<AVCodecContext,ContextFree>;
    using Frame=std::unique_ptr<AVFrame,FrameFree>;
    using Packet=std::unique_ptr<AVPacket,PacketFree>;
    auto *decoder=avcodec_find_decoder(AV_CODEC_ID_H264);auto *encoder=avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if(!decoder||!encoder)throw std::runtime_error("snapshot codecs unavailable");
    Context dec(avcodec_alloc_context3(decoder));Context enc(avcodec_alloc_context3(encoder));
    Frame raw(av_frame_alloc()),scaled(av_frame_alloc());Packet input(av_packet_alloc()),output(av_packet_alloc());
    if(!dec||!enc||!raw||!scaled||!input||!output)throw std::runtime_error("snapshot allocation");
    dec->thread_count=1;dec->thread_type=0;dec->max_pixels=1280*720;dec->err_recognition=AV_EF_EXPLODE;
    if(avcodec_open2(dec.get(),decoder,nullptr)<0||av_new_packet(input.get(),int(idr.size()))<0)throw std::runtime_error("snapshot decoder init");
    std::memcpy(input->data,idr.data(),idr.size());
    if(avcodec_send_packet(dec.get(),input.get())<0||avcodec_receive_frame(dec.get(),raw.get())<0)throw std::runtime_error("snapshot IDR decode");
    if(raw->width!=1280||raw->height!=720||(raw->format!=AV_PIX_FMT_YUV420P&&raw->format!=AV_PIX_FMT_YUVJ420P))throw std::runtime_error("snapshot decoded format");
    constexpr int width=320,height=180;
    scaled->format=AV_PIX_FMT_YUVJ420P;scaled->width=width;scaled->height=height;scaled->color_range=AVCOL_RANGE_JPEG;
    if(av_frame_get_buffer(scaled.get(),32)<0)throw std::runtime_error("snapshot scale buffer");
    std::unique_ptr<SwsContext,decltype(&sws_freeContext)> scale(sws_getContext(raw->width,raw->height,AVPixelFormat(raw->format),width,height,AV_PIX_FMT_YUVJ420P,SWS_BILINEAR,nullptr,nullptr,nullptr),sws_freeContext);
    if(!scale)throw std::runtime_error("snapshot scale init");
    const int *coeff=sws_getCoefficients(raw->colorspace==AVCOL_SPC_BT709?SWS_CS_ITU709:SWS_CS_DEFAULT);
    if(sws_setColorspaceDetails(scale.get(),coeff,raw->color_range==AVCOL_RANGE_JPEG,coeff,1,0,1<<16,1<<16)<0||sws_scale(scale.get(),raw->data,raw->linesize,0,raw->height,scaled->data,scaled->linesize)!=height)throw std::runtime_error("snapshot scaling");
    enc->width=width;enc->height=height;enc->pix_fmt=AV_PIX_FMT_YUVJ420P;enc->color_range=AVCOL_RANGE_JPEG;enc->time_base={1,1};enc->thread_count=1;enc->thread_type=0;enc->flags|=AV_CODEC_FLAG_QSCALE;
    const int quantizer=2+int((100-quality)*29/99);enc->global_quality=FF_QP2LAMBDA*quantizer;scaled->quality=enc->global_quality;
    if(avcodec_open2(enc.get(),encoder,nullptr)<0||avcodec_send_frame(enc.get(),scaled.get())<0||avcodec_receive_packet(enc.get(),output.get())<0)throw std::runtime_error("snapshot JPEG encode");
    if(output->size<4||output->size>64000||output->data[0]!=0xff||output->data[1]!=0xd8||output->data[output->size-2]!=0xff||output->data[output->size-1]!=0xd9)throw std::runtime_error("snapshot JPEG bounds");
    return {output->data,output->data+output->size};
}
}
