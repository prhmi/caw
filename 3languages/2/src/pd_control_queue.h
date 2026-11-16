#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

struct PdControlOutput {
    int clipId=0,trackId=0,kind=0; // 1 cawset, 2 cawscore
    std::int64_t frame=0;
    float value=0;
    char name[128]{};
};
// One worker produces, UI consumes. No locks or allocations on publish.
class PdControlQueue {
    static constexpr std::size_t capacity=4096;
    std::array<PdControlOutput,capacity> queue_{};
    std::atomic<std::size_t> head_{0},tail_{0};
    std::atomic<unsigned> dropped_{0};
public:
    void push(int clip,int track,std::int64_t frame,const std::string& kind,const std::string& name,float value) noexcept {
        if(name.size()>=sizeof(PdControlOutput::name)){dropped_.fetch_add(1);return;}
        auto head=head_.load(std::memory_order_relaxed),next=(head+1)%capacity;
        if(next==tail_.load(std::memory_order_acquire)){dropped_.fetch_add(1);return;}
        auto& item=queue_[head];item.clipId=clip;item.trackId=track;item.frame=frame;item.value=value;
        item.kind=kind=="cawset"?1:2;
        std::memcpy(item.name,name.c_str(),name.size()+1);
        head_.store(next,std::memory_order_release);
    }
    std::vector<PdControlOutput> take() {
        std::vector<PdControlOutput> result;
        auto tail=tail_.load(std::memory_order_relaxed),head=head_.load(std::memory_order_acquire);
        while(tail!=head){result.push_back(queue_[tail]);tail=(tail+1)%capacity;}
        tail_.store(tail,std::memory_order_release);return result;
    }
    unsigned takeDropped(){return dropped_.exchange(0);}
    void addDropped(unsigned n){if(n)dropped_.fetch_add(n,std::memory_order_relaxed);}
};
