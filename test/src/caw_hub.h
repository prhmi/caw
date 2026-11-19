#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

// CAW's shared message bus behind the caws (send) and cawr (receive) clip
// commands. One named number per name: Csound clips read and write it as the
// channel "caws/<name>" directly (all clips of one Run share the channels),
// and CAW mirrors every change to the running Pure Data instances (patches
// receive on [r $0-cawr-<name>]) and SuperCollider (dictionary ~cawr). Values
// are doubles; audio-rate or text messaging is out of scope.
class CawHub {
public:
    // Any thread. A value equal to the stored one is dropped, so polling
    // sources cannot flood the consumers.
    void send(const std::string& name,double value) {
        if(name.empty()||name.size()>120||!std::isfinite(value))return;
        {std::lock_guard<std::mutex> lock(mutex_);double& slot=values_[name];if(slot==value)return;slot=value;}
        revision_.fetch_add(1,std::memory_order_release);
    }
    // Consumer side: pass the revision you saw last; when something changed the
    // full value map is copied into `out` and true is returned.
    bool take(std::uint64_t& seen,std::map<std::string,double>& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::uint64_t rev=revision_.load(std::memory_order_acquire);
        if(rev==seen)return false;
        seen=rev;out=values_;return true;
    }
private:
    mutable std::mutex mutex_;
    std::map<std::string,double> values_;
    std::atomic<std::uint64_t> revision_{0};
};
