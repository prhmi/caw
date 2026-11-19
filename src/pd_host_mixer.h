#pragma once
#include "pd_daw_engine.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <map>
#include <set>

// PD is rendered by the Csound host worker, never by the device callback.
// This is one audio device and clock, but PD clips still use isolated libpd
// libraries, not a shared Pure Data namespace.
class PdHostMixer {
    struct Command { std::int64_t frame=0, loopStart=0, loopEnd=0; bool playing=false, loop=false; int serial=0; };
    PdIsolatedLibraries copies_; // must outlive sink's loaded libraries
    PdBackend empty_;
    PdDawEngine::Sink sink_;
    PdControlQueue controlQueue_;
    pd_frame_scheduler::Scheduler scheduler_;
    std::vector<float> samples_;
    std::vector<std::shared_ptr<PdDawEngine::Meter>> meters_;
    std::mutex meterMutex_;
    mutable std::mutex mutex_;
    Command command_;
    std::atomic<std::uint64_t> generation_{0};
    std::uint64_t seen_=0;
    std::uint64_t planGeneration_=0,seenPlan_=0;
    std::vector<pd_timeline::Event> plan_,pendingPlan_;
    std::string planError_;
    std::vector<std::string> notes_;
    std::vector<std::string> runtimeNotes_;
    Command current_;
    int channels_=0, rate_=0;
    std::string library_;
    std::atomic<std::int64_t> position_{-1};
    struct Preview {
        PdIsolatedLibraries copies;
        PdDawEngine::Sink sink;
        std::vector<float> tick;
        int cursor=64;
    };
    struct PreviewRequest { bool playing=false; pd_timeline::Event event{}; };
    std::map<int,std::unique_ptr<Preview>> previews_; // worker only
    std::mutex previewMutex_;
    std::map<int,PreviewRequest> previewRequests_;
    std::set<int> previewWanted_;
    std::vector<std::string> previewErrors_;
    static bool frame(double time,int rate,std::int64_t& result) {
        if(!std::isfinite(time)||time<0||time>static_cast<double>(INT64_MAX/rate)) return false;
        result=static_cast<std::int64_t>(std::llround(time*rate));return true;
    }
public:
    void replan(const std::vector<pd_timeline::Event>& plan) {
        std::lock_guard<std::mutex> lock(mutex_);pendingPlan_=plan;++planGeneration_;
    }
    std::string takePlanError() {
        std::lock_guard<std::mutex> lock(mutex_);std::string result;result.swap(planError_);return result;
    }
    std::vector<std::string> takePlanNotes() {
        std::lock_guard<std::mutex> lock(mutex_);std::vector<std::string> result;result.swap(notes_);return result;
    }
    std::vector<std::string> takeRuntimeNotes() {
        std::vector<std::string> result;
        result.swap(runtimeNotes_);
        return result;
    }
    std::vector<PdControlOutput> takeControlOutputs() {return controlQueue_.take();}
    unsigned takeDroppedControls(){return controlQueue_.takeDropped();}
    // CAW hub (caws/cawr): deliver shared bus values into every running PD clip
    // instance. Called on the Csound host worker between render blocks; a patch
    // that has no matching [r $0-cawr-<name>] simply ignores the message.
    void deliverHub(const std::map<std::string,double>& values) {
        std::string why;
        for(auto& clip:sink_.clips)if(clip.pd)
            for(const auto& entry:values)
                clip.pd->numberLocal("cawr-"+entry.first,static_cast<float>(entry.second),why);
    }
    void audition(const pd_timeline::Event& event,bool playing) {
        std::lock_guard<std::mutex> lock(previewMutex_);
        previewRequests_[event.clipId]={playing,event};
        if(playing)previewWanted_.insert(event.clipId);else previewWanted_.erase(event.clipId);
    }
    bool auditionRequested(int id) {
        std::lock_guard<std::mutex> lock(previewMutex_);
        return previewWanted_.count(id)!=0;
    }
    std::vector<std::string> takeAuditionErrors() {
        std::lock_guard<std::mutex> lock(previewMutex_);
        std::vector<std::string> result;result.swap(previewErrors_);return result;
    }
    // Called once, exclusively on the Csound host worker, before rendering.
    bool prepare(const std::string& library,const std::vector<pd_timeline::Event>& plan,
                 int rate,int channels,unsigned maxFrames,std::string& error) {
        if(rate<8000||rate>192000||channels<1||channels>8||maxFrames==0||maxFrames>65536) {
            error="Unsupported shared PD audio format.";return false;
        }
        channels_=channels; rate_=rate;library_=library;sink_.outputQueue=&controlQueue_;
        if(plan.empty()) {
            if(!empty_.openEngine(library,rate,channels,error))return false;
        } else if(!copies_.create(library,plan.size(),error))return false;
        std::vector<pd_frame_scheduler::Event> events;
        std::vector<pd_frame_scheduler::Control> controls;
        sink_.clips.reserve(plan.size());
        for(std::size_t i=0;i<plan.size();++i) {
            const auto& e=plan[i];std::int64_t start=0,end=0;
            if(!frame(e.start,rate,start)||!frame(e.end,rate,end)||end<=start) {
                error="PD clip "+std::to_string(e.clipId)+": invalid or sub-sample timing.";return false;
            }
            PdDawEngine::Clip clip;clip.event=e;clip.channels=channels;clip.sampleRate=rate;
            if(e.trackId>0) {
                for(const auto& meter:meters_)if(meter->trackId==e.trackId){clip.meter=meter;break;}
                if(!clip.meter){clip.meter=std::make_shared<PdDawEngine::Meter>(e.trackId);meters_.push_back(clip.meter);}
            }
            if(!pd_pfield_numeric::parse(e.pFieldRules,clip.rules,error)) {
                error="PD clip "+std::to_string(e.clipId)+": p-fields: "+error;return false;
            }
            clip.pd=std::make_unique<PdBackend>();
            if(!clip.pd->open(copies_.paths()[i],e.patch,rate,channels,error)) {
                error="PD clip "+std::to_string(e.clipId)+": "+error;return false;
            }
            if(!clip.pd->bindOutputs(e.outputs,error)) {
                error="PD clip "+std::to_string(e.clipId)+": outputs: "+error;return false;
            }
            sink_.collectDiagnostics(clip);
            events.push_back({e.clipId,start,end,e.muted});
            for(int r=0;r<e.runs;++r) {
                const auto span=end-start;
                const auto at=start+(span/e.runs)*r+(span%e.runs)*r/e.runs;
                if(at<end)controls.push_back({at,e.clipId,1,r,0});
            }
            if(e.midiEnabled)for(const auto& n:e.notes) {
                std::int64_t on=0,off=0;
                if(!frame(n.start,rate,on)||!frame(n.end,rate,off))continue;
                on=std::max(start,on);off=std::min(end,off);
                if(on>=end||off<=on)continue;
                controls.push_back({on,e.clipId,2,n.pitch,n.velocity});
                if(off<end)controls.push_back({off,e.clipId,3,n.pitch,0});
            }
            sink_.clips.push_back(std::move(clip));
        }
        if(!scheduler_.prepare(events,channels,error,std::move(controls)))return false;
        plan_=plan;
        samples_.resize(static_cast<std::size_t>(maxFrames)*channels);
        return true;
    }
    // UI publishes a transport request; only the host worker touches PD.
    void transport(bool playing,double time,double loopStart,double loopEnd,bool loop,int rate,int serial=0) {
        if(rate<=0)return;
        Command c;c.playing=playing;c.loop=loop;c.serial=serial;
        if(!frame(time,rate,c.frame)||!frame(loopStart,rate,c.loopStart)||!frame(loopEnd,rate,c.loopEnd))return;
        {std::lock_guard<std::mutex> lock(mutex_);command_=c;}
        generation_.fetch_add(1,std::memory_order_release);
    }
    // Render an arbitrary Csound ksmps block. Writes normalized PD samples.
    bool render(unsigned frames,const float*& result,std::string& error,int acknowledgedSerial=INT32_MAX) {
        if(static_cast<std::size_t>(frames)*channels_>samples_.size()){error="PD host block exceeds prepared size.";return false;}
        std::vector<pd_timeline::Event> nextPlan;
        bool hasPlan=false;
        {std::lock_guard<std::mutex> lock(mutex_);
            if(planGeneration_!=seenPlan_){nextPlan=pendingPlan_;seenPlan_=planGeneration_;hasPlan=true;}}
        if(hasPlan) {
            std::vector<pd_frame_scheduler::Event> events;
            std::vector<pd_frame_scheduler::Control> controls;
            std::string why;
            if(!pd_timeline::sameInstances(plan_,nextPlan))why="The set of PD clips changed; restart the engine.";
            else if(pd_plan_update::build(nextPlan,rate_,events,controls,why) &&
                    scheduler_.replan(events,std::move(controls),current_.playing,why)) {
                std::vector<std::string> liveNotes;
                sink_.applyPlan(nextPlan,rate_,liveNotes);
                if(!liveNotes.empty()){std::lock_guard<std::mutex> lock(mutex_);for(auto& n:liveNotes)notes_.push_back(std::move(n));}
                std::lock_guard<std::mutex> lock(meterMutex_);
                for(auto& clip:sink_.clips) {
                    const auto* e=pd_timeline::find(nextPlan,clip.event.clipId);
                    clip.meter.reset();
                    if(e->trackId>0) {
                        for(const auto& m:meters_)if(m->trackId==e->trackId){clip.meter=m;break;}
                        if(!clip.meter){clip.meter=std::make_shared<PdDawEngine::Meter>(e->trackId);meters_.push_back(clip.meter);}
                    }
                }
                plan_=std::move(nextPlan);
            }
            if(!why.empty()){std::lock_guard<std::mutex> lock(mutex_);planError_=why;}
        }
        const auto generation=generation_.load(std::memory_order_acquire);
        if(generation!=seen_) {
            Command next;
            {std::lock_guard<std::mutex> lock(mutex_);next=command_;}
            if(next.serial<=acknowledgedSerial) {
                current_=next;
                if(current_.loop && current_.loopEnd>current_.loopStart &&
                   (current_.frame<current_.loopStart||current_.frame>=current_.loopEnd))
                    current_.frame=current_.loopStart;
                scheduler_.seek(current_.frame,sink_);position_.store(current_.frame);
                seen_=generation;
            }
        }
        std::fill_n(samples_.data(),static_cast<std::size_t>(frames)*channels_,0.f);
        if(current_.playing) {
            unsigned done=0;
            while(done<frames) {
                auto begin=scheduler_.frame();
                if(current_.loop && current_.loopEnd>current_.loopStart && begin>=current_.loopEnd) {
                    scheduler_.seek(current_.loopStart,sink_);begin=scheduler_.frame();
                }
                unsigned count=frames-done;
                if(current_.loop && current_.loopEnd>current_.loopStart)
                    count=static_cast<unsigned>(std::min<std::int64_t>(count,current_.loopEnd-begin));
                if(!count){error="Invalid PD loop.";return false;}
                if(!scheduler_.render(samples_.data()+static_cast<std::size_t>(done)*channels_,count,sink_)) {
                    auto messages=sink_.takeRuntimeMessages();
                    for(auto& message:messages)runtimeNotes_.push_back(std::move(message));
                    error=sink_.error;return false;
                }
                auto messages=sink_.takeRuntimeMessages();
                for(auto& message:messages)runtimeNotes_.push_back(std::move(message));
                done+=count;
            }
            position_.store(scheduler_.frame(),std::memory_order_release);
            // Unity gain: PD in CAW must be exactly as loud as the same patch in Pure Data at the same
            // amplitude. The old *0.25 made Csound tracks 4x louder than PD tracks at equal amp; the
            // +-1 clamp stays as the only PD-bus protection.
            for(std::size_t i=0;i<static_cast<std::size_t>(frames)*channels_;++i)
                samples_[i]=std::max(-1.f,std::min(1.f,samples_[i]));
        }
        std::map<int,PreviewRequest> requests;
        {std::lock_guard<std::mutex> lock(previewMutex_);requests.swap(previewRequests_);}
        for(auto& entry:requests) {
            const int id=entry.first;
            const auto& request=entry.second;
            if(!request.playing) {
                auto it=previews_.find(id);
                if(it!=previews_.end()){it->second->sink.stop(id);previews_.erase(it);}
                continue;
            }
            auto next=std::make_unique<Preview>();
            PdDawEngine::Clip clip;clip.event=request.event;clip.channels=channels_;clip.sampleRate=rate_;
            std::string why;
            bool ok=pd_pfield_numeric::parse(clip.event.pFieldRules,clip.rules,why) &&
                    next->copies.create(library_,1,why);
            if(ok) {
                clip.pd=std::make_unique<PdBackend>();
                ok=clip.pd->open(next->copies.paths()[0],clip.event.patch,rate_,channels_,why);
            }
            if(ok) {
                next->sink.clips.push_back(std::move(clip));
                next->tick.resize(static_cast<std::size_t>(64)*channels_);
                ok=next->sink.start(id) && next->sink.control({0,id,1,0,0});
                if(!ok)why=next->sink.error;
            }
            if(ok) {
                auto old=previews_.find(id);
                if(old!=previews_.end())old->second->sink.stop(id);
                previews_[id]=std::move(next);
            } else {
                std::lock_guard<std::mutex> lock(previewMutex_);
                // A failed reload leaves the previous sounding instance alive.
                if(previews_.find(id)==previews_.end())previewWanted_.erase(id);
                previewErrors_.push_back("PD clip "+std::to_string(id)+": audition could not load: "+why);
            }
        }
        for(auto it=previews_.begin();it!=previews_.end();) {
            auto& preview=*it->second;
            bool ok=true;
            for(unsigned f=0;f<frames&&ok;++f) {
                if(preview.cursor==64) {
                    ok=preview.sink.tick(it->first,preview.tick.data(),channels_);
                    preview.cursor=0;
                }
                if(ok)for(int c=0;c<channels_;++c)
                    samples_[static_cast<std::size_t>(f)*channels_+c]+=
                        preview.tick[static_cast<std::size_t>(preview.cursor)*channels_+c];
                ++preview.cursor;
            }
            for(auto& message:preview.sink.takeRuntimeMessages())
                runtimeNotes_.push_back(std::move(message));
            if(!ok) {
                const int id=it->first;
                {std::lock_guard<std::mutex> lock(previewMutex_);
                    previewWanted_.erase(id);previewErrors_.push_back("PD clip "+std::to_string(id)+": audition stopped: "+preview.sink.error);}
                preview.sink.stop(id);it=previews_.erase(it);
            } else ++it;
        }
        // Preload and live-reload diagnostics exist even while transport is
        // stopped. Publish them without waiting for a timeline render.
        for(auto& message:sink_.takeRuntimeMessages())
            runtimeNotes_.push_back(std::move(message));
        // Audition previews are added after the timeline clamp, so clamp the whole PD bus once here:
        // the runner adds this buffer to Csound's output as-is.
        for(std::size_t i=0;i<static_cast<std::size_t>(frames)*channels_;++i)
            samples_[i]=std::max(-1.f,std::min(1.f,samples_[i]));
        result=samples_.data();return true;
    }
    std::int64_t position() const {return position_.load(std::memory_order_acquire);}
    // Called on the same host worker that loaded and ran libpd.
    void shutdown() {
        scheduler_.seek(scheduler_.frame(),sink_);
        for(auto& preview:previews_)preview.second->sink.stop(preview.first);
        previews_.clear();
        sink_.clips.clear();
        empty_.close();
        copies_.cleanup();
    }
    std::vector<PdDawEngine::TrackPeak> takePeaks() {
        std::vector<PdDawEngine::TrackPeak> result;
        std::lock_guard<std::mutex> lock(meterMutex_);
        for(const auto& meter:meters_)result.push_back({meter->trackId,{meter->peak[0].exchange(0.f),meter->peak[1].exchange(0.f)}});
        return result;
    }
};
