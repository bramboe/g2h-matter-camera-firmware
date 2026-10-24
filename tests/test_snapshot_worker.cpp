// Host test: worker timing, caching and failure bounds without FFmpeg or Matter.
#include "../app/src/g2h_snapshot_worker.hpp"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <thread>
using namespace g2h;
using namespace std::chrono;
using SC=SnapshotWorker::SteadyClock;
struct Fake {
    std::mutex m;std::vector<std::uint8_t> idr;SC::time_point at{};
    std::atomic<int> encodes{0};std::atomic<int> delayMs{0};std::atomic<bool> fail{false};
    void Set(std::uint8_t tag,SC::time_point when=SC::now()){std::lock_guard<std::mutex> l(m);idr={0,0,0,1,tag};at=when;}
    SnapshotWorker::Source Source(){return [this](std::vector<std::uint8_t>&out,SC::time_point&when){
        std::lock_guard<std::mutex> l(m);if(idr.empty())return false;out=idr;when=at;return true;};}
    SnapshotWorker::Encode Encode(){return [this](const std::vector<std::uint8_t>&in,unsigned q){
        ++encodes;std::this_thread::sleep_for(milliseconds(delayMs.load()));
        if(fail)throw std::runtime_error("decode");
        return std::vector<std::uint8_t>{255,216,in.back(),std::uint8_t(q),255,217};};}
};
static milliseconds Elapsed(SC::time_point t){return duration_cast<milliseconds>(SC::now()-t);}
int main(){
    assert(SnapshotDimensions(320,180)&&SnapshotDimensions(640,360));
    assert(!SnapshotDimensions(640,180)&&!SnapshotDimensions(1280,720));
    assert(SnapshotFits(640,360,320,180,640,360));
    assert(!SnapshotFits(640,360,320,180,320,180));
    assert(!SnapshotFits(320,180,640,360,640,360));

    {   // Not started: never blocks, never succeeds.
        Fake f;SnapshotWorker w(f.Source(),f.Encode());std::vector<std::uint8_t> out;
        auto t=SC::now();assert(!w.Get(70,milliseconds(200),out));assert(Elapsed(t)<milliseconds(50));
    }
    {   // No IDR: Get gives up after its own wait, not the worker budget.
        Fake f;SnapshotWorker w(f.Source(),f.Encode());assert(w.Start());std::vector<std::uint8_t> out;
        auto t=SC::now();assert(!w.Get(70,milliseconds(100),out));auto e=Elapsed(t);
        assert(e>=milliseconds(90)&&e<milliseconds(250));assert(f.encodes==0);
        // The IDR arrives later within the request budget: the worker picks it up.
        f.Set(7);std::this_thread::sleep_for(milliseconds(150));
        t=SC::now();assert(w.Get(70,milliseconds(0),out));assert(Elapsed(t)<milliseconds(20));
        assert(out.size()==6&&out[2]==7&&out[3]==70);assert(f.encodes==1);
    }
    {   // Prefetch then cached reads: one encode per IDR and quality.
        Fake f;f.Set(1);SnapshotWorker w(f.Source(),f.Encode());assert(w.Start());std::vector<std::uint8_t> out;
        w.Request(70);std::this_thread::sleep_for(milliseconds(80));
        for(int n=0;n<5;++n)assert(w.Get(70,milliseconds(0),out));
        assert(f.encodes==1);
        assert(w.Get(40,milliseconds(500),out)&&out[3]==40);assert(f.encodes==2);
    }
    {   // Slow encode (live session budget 300 ms): Get returns on time, worker finishes later.
        Fake f;f.Set(2);f.delayMs=600;SnapshotWorker w(f.Source(),f.Encode());assert(w.Start());std::vector<std::uint8_t> out;
        auto t=SC::now();assert(!w.Get(70,milliseconds(300),out));auto e=Elapsed(t);
        assert(e>=milliseconds(290)&&e<milliseconds(400));
        assert(w.Get(70,milliseconds(600),out)&&out[2]==2);assert(f.encodes==1);
    }
    {   // Stale cache is not served; a newer IDR is encoded instead.
        Fake f;f.Set(3);SnapshotWorker w(f.Source(),f.Encode(),milliseconds(100));assert(w.Start());std::vector<std::uint8_t> out;
        assert(w.Get(70,milliseconds(300),out)&&out[2]==3);
        std::this_thread::sleep_for(milliseconds(150));assert(!w.Get(70,milliseconds(0),out));
        f.Set(4);assert(w.Get(70,milliseconds(300),out)&&out[2]==4);
    }
    {   // Undecodable IDR: no crash and no hot retry loop on the same unit.
        Fake f;f.Set(5);f.fail=true;SnapshotWorker w(f.Source(),f.Encode(),seconds(3),milliseconds(400));assert(w.Start());std::vector<std::uint8_t> out;
        assert(!w.Get(70,milliseconds(450),out));assert(f.encodes==1);
        f.fail=false;f.Set(6);assert(w.Get(70,milliseconds(300),out)&&out[2]==6);
    }
    {   // Throwing source is contained.
        SnapshotWorker w([](std::vector<std::uint8_t>&,SC::time_point&)->bool{throw std::runtime_error("rtsp");},
            [](const std::vector<std::uint8_t>&,unsigned){return std::vector<std::uint8_t>{1};});
        assert(w.Start());std::vector<std::uint8_t> out;assert(!w.Get(70,milliseconds(60),out));
    }
    {   // Stop during an encode joins and wakes a waiting Get; restart works.
        Fake f;f.Set(8);f.delayMs=300;SnapshotWorker w(f.Source(),f.Encode());assert(w.Start());
        std::atomic<bool> returned{false};std::vector<std::uint8_t> out;
        std::thread waiter([&]{std::vector<std::uint8_t> o;(void)w.Get(70,seconds(5),o);returned=true;});
        std::this_thread::sleep_for(milliseconds(50));auto t=SC::now();w.Stop();waiter.join();
        assert(returned&&Elapsed(t)<milliseconds(600));
        assert(!w.Get(70,milliseconds(10),out));
        f.delayMs=0;assert(w.Start()&&w.Get(70,milliseconds(300),out));
    }
    {   // Concurrent readers on a warm cache.
        Fake f;f.Set(9);SnapshotWorker w(f.Source(),f.Encode());assert(w.Start());std::vector<std::uint8_t> warm;
        assert(w.Get(70,milliseconds(300),warm));std::atomic<int> ok{0};std::vector<std::thread> ts;
        for(int n=0;n<4;++n)ts.emplace_back([&]{std::vector<std::uint8_t> o;for(int k=0;k<200;++k)if(w.Get(70,milliseconds(0),o))++ok;});
        for(auto &t:ts)t.join();assert(ok==800&&f.encodes==1);
    }
    {   // The same shared IDR can fill either size; a cache hit requires size+quality.
        Fake f;f.Set(10);std::atomic<unsigned> sizedEncodes{0};
        SnapshotWorker w(f.Source(),[&](const std::vector<uint8_t>&,unsigned q,unsigned width,unsigned height){
            ++sizedEncodes;return std::vector<uint8_t>{uint8_t(width/10),uint8_t(height/10),uint8_t(q)};
        });assert(w.Start());std::vector<uint8_t> out;
        assert(w.Get(70,milliseconds(300),out)&&out[0]==32&&out[1]==18);
        assert(w.Get(70,milliseconds(300),out,640,360)&&out[0]==64&&out[1]==36);
        assert(sizedEncodes==2);assert(w.Get(70,milliseconds(0),out,640,360)&&sizedEncodes==2);
        assert(w.Get(70,milliseconds(300),out)&&out[0]==32&&sizedEncodes==3);
        auto n=sizedEncodes.load();assert(!w.Get(70,milliseconds(100),out,640,180));assert(sizedEncodes==n);
    }
    {   // Over-cap 640 JPEG cannot poison or masquerade as a valid 320 cache.
        Fake f;f.Set(11);std::atomic<unsigned> largeEncodes{0};
        SnapshotWorker w(f.Source(),[&](const std::vector<uint8_t>&,unsigned,unsigned width,unsigned){
            if(width==640){++largeEncodes;return std::vector<uint8_t>(64001,1);}
            return std::vector<uint8_t>{1,2,3};
        },seconds(3),milliseconds(120));assert(w.Start());std::vector<uint8_t> out;
        assert(!w.Get(70,milliseconds(180),out,640,360));assert(largeEncodes==1);
        assert(w.Get(70,milliseconds(200),out)&&out.size()==3); // Same IDR, different key succeeds.
        assert(!w.Get(70,milliseconds(150),out,640,360));assert(largeEncodes==1);
    }
    {   // Exactly64000 is permitted; larger results never enter the cache.
        Fake f;f.Set(12);SnapshotWorker w(f.Source(),[](const std::vector<uint8_t>&,unsigned,unsigned,unsigned){return std::vector<uint8_t>(64000,1);});
        assert(w.Start());std::vector<uint8_t> out;assert(w.Get(70,milliseconds(300),out,640,360)&&out.size()==64000);
    }
    {   // Expired and oversize IDRs never reach the encoder or refill a stale cache.
        Fake f;f.Set(13,SC::now()-seconds(5));SnapshotWorker w(f.Source(),f.Encode(),milliseconds(100),milliseconds(100));
        assert(w.Start());std::vector<uint8_t> out;assert(!w.Get(70,milliseconds(140),out));assert(f.encodes==0);
        {std::lock_guard<std::mutex> l(f.m);f.at=SC::now();f.idr.resize(1024*1024+1);}
        assert(!w.Get(70,milliseconds(140),out));assert(f.encodes==0);
    }
    std::puts("snapshot worker tests passed");
}
