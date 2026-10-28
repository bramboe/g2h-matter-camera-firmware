#pragma once
#include "g2h_media_input.hpp"
#include "h264_geometry.hpp"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#ifdef G2H_VIDEO_SOURCE_TESTING
#include <functional>
#endif

namespace g2h {
// The worker owns one loopback RTSP client. It never touches the vendor process,
// GPIO, UART or host camera configuration. Frames remain in bounded RAM queues.
class VideoSource {
public:
    ~VideoSource(){Stop();}
    VideoSource()=default;
    VideoSource(const VideoSource&)=delete;
    VideoSource& operator=(const VideoSource&)=delete;
    bool Start(){
        // Reuse the one ingest connection across Matter consumers.
        if(worker_.joinable()&&!stop_&&!fault_)return true;
        // At most one new RTSP connection per 15 s after a failure: the vendor server has
        // hung on rapid connect/close cycles before.
        if(fault_&&Clock::now()-failedAt_.load()<std::chrono::seconds(15))return false;
        Stop();fault_=false;stop_=false;streaming_=false;
        try{
            auto source=std::make_unique<MediaInput>(false);source->Start();
            if(!IsSupported720pSps(source->parameters.sps)){fault_=true;stop_=true;failedAt_=Clock::now();return false;}
            {std::lock_guard<std::mutex> lock(mutex_);profile_=source->parameters.ProfileLevel();}
            worker_=std::thread([this,source=std::move(source)]()mutable{Run(*source);});
            return true;
        }catch(...){fault_=true;stop_=true;failedAt_=Clock::now();return false;}
    }
    void Stop() noexcept {
        stop_=true;streaming_=false;
        if(worker_.joinable())worker_.join();
        std::lock_guard<std::mutex> lock(mutex_);queue_.clear();bytes_=0;profile_.clear();latestIdr_.clear();idrAt_=Clock::time_point{};
    }
    void EnableFrames(bool enable){
        std::lock_guard<std::mutex> lock(mutex_);
        if(streaming_.exchange(enable)!=enable){++generation_;queue_.clear();bytes_=0;}
    }
    // Does not stop/reopen the subscriber or broker/native RTSP ingest. Keep
    // draining input while forbidden; erase both live and snapshot copies.
    // Epochs reject an AU whose read/assembly straddles either transition.
    void SetGlobalAllowed(bool allowed){
        std::lock_guard<std::mutex> lock(mutex_);
        if(globalAllowed_.load()==allowed)return;
        const auto now=Clock::now();
        if(allowed)pausedElapsed_+=now-pausedSince_;else pausedSince_=now;
        globalAllowed_=allowed;
        ++globalEpoch_;++generation_;queue_.clear();bytes_=0;latestIdr_.clear();idrAt_={};
    }
#ifdef G2H_VIDEO_SOURCE_TESTING
    // Host-only scheduling seam: pause one already-started record read to
    // exercise privacy transitions while an AU is genuinely in flight.
    std::function<void()> beforePayloadRead;
#endif
    bool GlobalAllowed() const {return globalAllowed_;}
    std::uint64_t Epoch() const {return globalEpoch_;}
    bool Pop(H264Rtp::Frame &frame){
        std::lock_guard<std::mutex> lock(mutex_);if(!globalAllowed_||!streaming_||stop_||fault_||queue_.empty())return false;
        frame=std::move(queue_.front());queue_.pop_front();bytes_-=frame.annexB.size();return true;
    }
    bool LatestIdr(std::vector<std::uint8_t>& out) {Clock::time_point at;return LatestIdr(out,at);}
    bool LatestIdr(std::vector<std::uint8_t>& out,Clock::time_point& at) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!globalAllowed_||stop_||fault_||latestIdr_.empty()||Clock::now()-idrAt_>std::chrono::seconds(3))return false;
        out=latestIdr_;at=idrAt_;return true;
    }
    bool Faulted()const{return fault_;}
    std::string Profile(){std::lock_guard<std::mutex> lock(mutex_);return profile_;}
private:
    std::chrono::nanoseconds PausedTime(){
        std::lock_guard<std::mutex> lock(mutex_);
        return pausedElapsed_+(globalAllowed_?Clock::duration::zero():Clock::now()-pausedSince_);
    }
    void Run(MediaInput &source) noexcept {
        try{
            H264Rtp parser;H264Rtp::Frame frame;std::vector<std::uint8_t> packet;
            bool awaitIdr=true;std::uint64_t generation=generation_,parserEpoch=globalEpoch_;
            while(!stop_){
                const auto epoch=globalEpoch_.load();
                if(parserEpoch!=epoch){parser=H264Rtp{};frame={};awaitIdr=true;parserEpoch=epoch;}
                source.RenewReadDeadline();const auto pauseAtRecordStart=PausedTime();
                const auto pauseClock=[this]{return PausedTime();};
                std::uint8_t header[4];source.ReadWithCapturePause(header,4,globalAllowed_,stop_,pauseClock,pauseAtRecordStart);
                if(header[0]!='$')throw std::runtime_error("RTSP record");
                packet.resize((header[2]<<8)|header[3]);if(packet.empty())throw std::runtime_error("empty RTP");
#ifdef G2H_VIDEO_SOURCE_TESTING
                if(beforePayloadRead)beforePayloadRead();
#endif
                source.ReadWithCapturePause(packet.data(),packet.size(),globalAllowed_,stop_,pauseClock,pauseAtRecordStart);
                if(!globalAllowed_||epoch!=globalEpoch_.load()){
                    parser=H264Rtp{};frame={};awaitIdr=true;continue;
                }
                if(header[1]!=0)continue;
                auto result=parser.Push(packet.data(),packet.size(),frame);
                if(result==H264Rtp::Result::Rejected){
                    std::lock_guard<std::mutex> lock(mutex_);
                    queue_.clear();bytes_=0;awaitIdr=true;continue;
                }
                if(result!=H264Rtp::Result::FrameReady)continue;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if(!globalAllowed_||epoch!=globalEpoch_.load()){
                        parser=H264Rtp{};frame={};awaitIdr=true;continue;
                    }
                    if(frame.idr){
                        if(!source.parameters.PrependToIdr(frame))throw std::runtime_error("parameter insertion");
                        latestIdr_=frame.annexB;idrAt_=Clock::now();
                    }
                    if(generation!=generation_.load()||!streaming_){queue_.clear();bytes_=0;awaitIdr=true;generation=generation_;}
                    if(!streaming_){frame=H264Rtp::Frame{};continue;}
                    if(queue_.size()>=32||bytes_+frame.annexB.size()+4096>2*1024*1024){queue_.clear();bytes_=0;awaitIdr=true;}
                    if(awaitIdr&&!frame.idr){frame=H264Rtp::Frame{};continue;}
                    // IDR parameters were already inserted before caching.
                    awaitIdr=false;bytes_+=frame.annexB.size();queue_.push_back(std::move(frame));frame=H264Rtp::Frame{};
                }
            }
        }catch(...){if(!stop_){failedAt_=Clock::now();fault_=true;}}
    }
    std::atomic<bool> stop_{true},streaming_{false},fault_{false},globalAllowed_{true};
    std::atomic<std::uint64_t> generation_{0},globalEpoch_{0};
    std::thread worker_;
    std::mutex mutex_;
    Clock::duration pausedElapsed_{};Clock::time_point pausedSince_{};
    std::deque<H264Rtp::Frame> queue_;
    std::size_t bytes_=0;
    std::string profile_;
    std::vector<std::uint8_t> latestIdr_;
    Clock::time_point idrAt_{};
    std::atomic<Clock::time_point> failedAt_{Clock::time_point{}};
};
}
