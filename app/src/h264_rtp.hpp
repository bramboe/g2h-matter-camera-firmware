#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace g2h {
// H.264 RTP to Annex-B access units, the input used by libdatachannel.
// The caller owns delivery/storage. This class never writes media to disk.
class H264Rtp {
public:
    static constexpr std::size_t kMaxFrame = 512 * 1024;
    struct Frame {
        std::vector<std::uint8_t> annexB;
        std::uint32_t timestamp = 0;
        bool idr = false;
        unsigned nals = 0;
    };
    enum class Result { Pending, FrameReady, Rejected, OtherPayload };
    Result Push(const std::uint8_t *p, std::size_t n, Frame &out) {
        if (n < 12 || p[0] >> 6 != 2) return Reject();
        std::size_t h = 12 + (p[0] & 15) * 4;
        if (h > n) return Reject();
        if (p[0] & 16) {
            if (n - h < 4) return Reject();
            std::size_t words = (std::size_t(p[h+2]) << 8) | p[h+3];
            if (words > (n-h-4)/4) return Reject();
            h += 4 + words*4;
        }
        if (p[0] & 32) {
            unsigned padding = p[n-1];
            if (!padding || padding > n-h) return Reject();
            n -= padding;
        }
        if ((p[1] & 127) != 36) return Result::OtherPayload;
        if (h >= n) return Reject();
        auto seq = std::uint16_t((p[2] << 8) | p[3]);
        auto stamp = Be32(p+4), ssrc = Be32(p+8);
        if (haveSequence_ && (seq != next_ || ssrc != ssrc_)) {
            ResetFrame(); broken_=true; brokenStamp_=stamp;
        }
        if (broken_) {
            if (stamp == brokenStamp_) {haveSequence_=true;next_=std::uint16_t(seq+1);ssrc_=ssrc;return Result::Rejected;}
            broken_=false;
        }
        if (!frame_.annexB.empty() && stamp != frame_.timestamp) ResetFrame();
        haveSequence_ = true; next_ = std::uint16_t(seq+1); ssrc_ = ssrc;
        frame_.timestamp = stamp;
        const auto *b = p+h; std::size_t count = n-h;
        unsigned type = b[0] & 31;
        if (b[0] & 128) return Reject();
        if (type >= 1 && type <= 23) {
            if (fragment_ || !Nal(b, count)) return Reject();
        } else if (type == 24) {
            if (fragment_) return Reject();
            // Validate every aggregate member before committing any bytes.
            std::size_t at=1, total=0; unsigned members=0;
            while (at<count) {
                if (count-at<2) return Reject();
                std::size_t len=(b[at]<<8)|b[at+1]; at+=2;
                if (!len || len>count-at || (b[at]&128) || !(b[at]&31) || (b[at]&31)>23) return Reject();
                total += len+4; if (total>kMaxFrame || ++members>64) return Reject();
                at+=len;
            }
            if (!members || frame_.annexB.size()+total>kMaxFrame) return Reject();
            for (at=1;at<count;) { std::size_t len=(b[at]<<8)|b[at+1]; at+=2; if(!Nal(b+at,len))return Reject();at+=len; }
        } else if (type == 28) {
            if (count < 3 || (b[1]&32) || !(b[1]&31) || (b[1]&31)>23) return Reject();
            bool start=b[1]&128, end=b[1]&64;
            if (start && end) return Reject();
            if (start) {
                if (fragment_) return Reject();
                std::uint8_t header=(b[0]&224)|(b[1]&31);
                if (!Nal(&header,1)) return Reject();
                fragment_=true; fragmentHeader_=header;
            } else if (!fragment_ || fragmentHeader_ != ((b[0]&224)|(b[1]&31))) return Reject();
            if (frame_.annexB.size()+count-2>kMaxFrame) return Reject();
            frame_.annexB.insert(frame_.annexB.end(),b+2,b+count);
            if (end) fragment_=false;
        } else return Reject();
        if (p[1]&128) {
            if (fragment_ || frame_.annexB.empty()) return Reject();
            out = std::move(frame_); frame_=Frame{}; return Result::FrameReady;
        }
        return Result::Pending;
    }
private:
    static std::uint32_t Be32(const std::uint8_t *p){return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(p[2]<<8)|p[3];}
    bool Nal(const std::uint8_t *p,std::size_t n) {
        if (frame_.annexB.size()+4+n>kMaxFrame) return false;
        frame_.annexB.insert(frame_.annexB.end(),{0,0,0,1});
        frame_.annexB.insert(frame_.annexB.end(),p,p+n);
        ++frame_.nals; frame_.idr |= (p[0]&31)==5; return true;
    }
    void ResetFrame(){frame_=Frame{};fragment_=false;}
    void Reset(){ResetFrame();haveSequence_=false;}
    Result Reject(){Reset();return Result::Rejected;}
    Frame frame_;
    bool fragment_=false,haveSequence_=false;
    std::uint8_t fragmentHeader_=0;
    std::uint16_t next_=0;
    std::uint32_t ssrc_=0;
    bool broken_=false;
    std::uint32_t brokenStamp_=0;
};
}
