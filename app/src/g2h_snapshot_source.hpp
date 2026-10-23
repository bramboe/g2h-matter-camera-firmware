#pragma once
#include "g2h_rtsp_source.hpp"
#include "h264_geometry.hpp"
#include "g2h_snapshot_codec.hpp"
namespace g2h {
inline std::vector<std::uint8_t> CaptureSnapshotFromRtsp(unsigned quality){
    const auto began=Clock::now();Rtsp source("127.0.0.1",std::chrono::milliseconds(2500),true);source.Start();
    if(!IsSupported720pSps(source.parameters.sps))throw std::runtime_error("snapshot source format");
    H264Rtp parser;H264Rtp::Frame frame;std::vector<std::uint8_t> packet;std::size_t total=0;
    for(unsigned count=0;count<2048&&Clock::now()-began<std::chrono::milliseconds(2500);++count){
        std::uint8_t head[4];source.Read(head,4);if(head[0]!='$')throw std::runtime_error("snapshot RTSP record");
        std::size_t n=(head[2]<<8)|head[3];total+=n;if(!n||total>2*1024*1024)throw std::runtime_error("snapshot RTP bounds");
        packet.resize(n);source.Read(packet.data(),n);if(head[1]!=0)continue;
        if(parser.Push(packet.data(),n,frame)!=H264Rtp::Result::FrameReady||!frame.idr)continue;
        if(!source.parameters.PrependToIdr(frame))throw std::runtime_error("snapshot SPS/PPS bounds");
        source.Stop();return SnapshotJpeg(frame.annexB,quality);
    }
    throw std::runtime_error("snapshot IDR deadline");
}
}
