#pragma once
#include "pd_timeline.h"
#include "pd_frame_scheduler.h"
#include <climits>

namespace pd_plan_update {
inline bool toFrame(double time,int rate,std::int64_t& out) {
    if(rate<=0||!std::isfinite(time)||time<0||time>static_cast<double>(INT64_MAX/rate))return false;
    out=static_cast<std::int64_t>(std::llround(time*rate));return true;
}
inline bool build(const std::vector<pd_timeline::Event>& plan,int rate,
                  std::vector<pd_frame_scheduler::Event>& events,
                  std::vector<pd_frame_scheduler::Control>& controls,std::string& error) {
    events.clear();controls.clear();
    for(const auto& e:plan) {
        std::int64_t start=0,end=0;
        if(e.clipId<=0||e.runs<1||e.runs>256||!toFrame(e.start,rate,start)||
           !toFrame(e.end,rate,end)||end<=start) {error="PD clip has invalid timing.";return false;}
        events.push_back({e.clipId,start,end});
        for(int r=0;r<e.runs;++r) {
            const auto span=end-start;
            const auto at=start+(span/e.runs)*r+(span%e.runs)*r/e.runs;
            if(at<end)controls.push_back({at,e.clipId,1,r,0});
        }
        if(e.midiEnabled)for(const auto& n:e.notes) {
            std::int64_t on=0,off=0;
            if(!toFrame(n.start,rate,on)||!toFrame(n.end,rate,off))continue;
            on=std::max(start,on);off=std::min(end,off);
            if(on>=end||off<=on)continue;
            controls.push_back({on,e.clipId,2,n.pitch,n.velocity});
            if(off<end)controls.push_back({off,e.clipId,3,n.pitch,0});
        }
    }
    error.clear();return true;
}
}
