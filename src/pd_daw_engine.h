#pragma once
#include "pd_cables.h"
#include "track_gain.h"
#include "audio_output.h"
#include "caw_hub.h"
#include "pd_live_ring.h"
#include "pd_backend.h"
#include "pd_frame_scheduler.h"
#include "pd_isolated_libraries.h"
#include "pd_pfield_numeric.h"
#include "pd_plan_update.h"
#include "pd_control_queue.h"
#include "pd_timeline.h"
#include "pd_channels.h"
#include "caw_audio_bus.h"
#include "caw_audio_in.h"
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

// Dedicated worker preloads one isolated libpd per clip and renders timeline
// boundaries by frame. Only the worker calls libpd; PortAudio copies samples.
class PdDawEngine {
public:
    struct Meter {
        static constexpr int CH = 8;   // a patch's dac~ can use any width; every channel gets its own peak
        int trackId;
        std::array<std::atomic<float>,CH> peak;
        explicit Meter(int id):trackId(id){for(auto& p:peak)p.store(0.f);}
    };
    struct Clip {
        pd_timeline::Event event;
        std::vector<pd_pfield_numeric::Rule> rules;
        std::unique_ptr<PdBackend> pd;
        std::shared_ptr<Meter> meter;
        int channels=2;
        int sampleRate=48000;
        std::int64_t timelineFrame=0;
        std::uint64_t silentFrames=0;
        bool silenceNoticeSent=false;
        bool notes[128]{};
        bool active=false;   // between start() and stop(): the patch is sounding
        // CAW AUDIO BUSES: one port per cawbrecv / cawbsend line of the patch (created on first tick)
        std::vector<std::pair<pd_control_syntax::BusLink,std::shared_ptr<cawbus::Port>>> busPorts;
        std::vector<float> busIn, busLr, cableOut;   // 64 * kInputChannels inputs, 64 * 2 stereo scratch, 64 * 16 cable sends
        cawin::Reader mic;                            // this clip's read position in the soundcard input
    };
    class Sink : public pd_frame_scheduler::Sink {
    public:
        std::vector<Clip> clips;
        PdControlQueue* outputQueue=nullptr;
        std::string error;
        std::vector<std::string> runtimeMessages;
        Clip* find(int id){for(auto& c:clips)if(c.event.clipId==id)return &c;return nullptr;}
        void collectDiagnostics(Clip& c) {
            if(!c.pd)return;
            for(const auto& message:c.pd->takeDiagnostics())
                runtimeMessages.push_back("PD clip "+std::to_string(c.event.clipId)+": "+message);
        }
        std::vector<std::string> takeRuntimeMessages() {
            std::vector<std::string> result;
            result.swap(runtimeMessages);
            return result;
        }
        static void number(Clip& c,const std::string& name,float v){std::string ignored;c.pd->numberLocal(name,v,ignored);}
        static void bang(Clip& c,const std::string& name){std::string ignored;c.pd->bangLocal(name,ignored);}
        void setFrame(int id,std::int64_t frame) override {auto* c=find(id);if(c)c->timelineFrame=frame;}
        static double automationValue(const pd_timeline::Automation& a,double time) {
            if(!a.enabled||a.points.empty()||time<a.points.front().t)return a.fallback;
            if(time>=a.points.back().t)return a.points.back().v;
            for(std::size_t i=0;i+1<a.points.size();++i) {
                const auto& x=a.points[i];const auto& y=a.points[i+1];
                if(time<=y.t) {
                    if(y.t<=x.t)return y.v;
                    return x.v+(y.v-x.v)*(time-x.t)/(y.t-x.t);
                }
            }
            return a.points.back().v;
        }
        bool start(int id) override {
            auto* c=find(id);if(!c){error="PD clip "+std::to_string(id)+": unknown clip instance.";return false;}
            c->active=true;
            c->silentFrames=0;c->silenceNoticeSent=false;
            number(*c,"caw-p1",static_cast<float>(id));
            number(*c,"caw-p2",static_cast<float>(c->event.start));
            number(*c,"caw-p3",static_cast<float>(c->event.end-c->event.start));
            number(*c,"caw-gate",1.f);bang(*c,"caw-start");collectDiagnostics(*c);return true;
        }
        bool tick(int id,float* out,int) override {
            auto* c=find(id);
            if(c)for(const auto& a:c->event.automation)
                number(*c,"cawget-"+a.name,static_cast<float>(automationValue(a,
                    static_cast<double>(c->timelineFrame)/std::max(1,c->sampleRate))));
            if(!c){error="PD clip "+std::to_string(id)+": unknown clip instance.";return false;}
            // INPUTS: [adc~ 1..8] = the soundcard input (microphone), [adc~ 9..24] = the CAW cables
            // this patch receives with [cawr~ name]. OUTPUTS: [dac~ 1..N] = the track, then the 16
            // channels [caws~ name] writes into its cables (never the master).
            if(c->busIn.size()!=(size_t)64*PdBackend::kInputChannels)c->busIn.assign((size_t)64*PdBackend::kInputChannels,0.f);
            if(c->busLr.size()<128)c->busLr.assign(128,0.f);
            if(c->cableOut.size()!=(size_t)64*PdBackend::kCableOutChannels)c->cableOut.assign((size_t)64*PdBackend::kCableOutChannels,0.f);
            if(c->busPorts.size()!=c->event.buses.size()) {
                c->busPorts.clear();
                for(const auto& link:c->event.buses){auto p=std::make_shared<cawbus::Port>();p->busName=link.bus;c->busPorts.push_back({link,p});}
            }
            {
                std::fill(c->busIn.begin(),c->busIn.end(),0.f);
                static thread_local std::vector<float> micBlock;
                if(micBlock.size()<(size_t)64*PdBackend::kMicChannels)micBlock.resize((size_t)64*PdBackend::kMicChannels);
                c->mic.read(micBlock.data(),64,PdBackend::kMicChannels);
                for(int n=0;n<64;++n)for(int k=0;k<PdBackend::kMicChannels;++k)
                    c->busIn[(size_t)n*PdBackend::kInputChannels+k]=micBlock[(size_t)n*PdBackend::kMicChannels+k];
                for(auto& bp:c->busPorts) {
                    if(bp.first.send)continue;
                    cawbus::read(*bp.second,c->busLr.data(),64);
                    const int l=PdBackend::kMicChannels+(bp.first.channel-1)*2;
                    for(int n=0;n<64;++n){c->busIn[(size_t)n*PdBackend::kInputChannels+l]=c->busLr[(size_t)n*2];c->busIn[(size_t)n*PdBackend::kInputChannels+l+1]=c->busLr[(size_t)n*2+1];}
                }
            }
            if(!c->pd->processTick(out,error,c->busIn.data(),c->cableOut.data())){
                collectDiagnostics(*c);
                error="PD clip "+std::to_string(c->event.clipId)+": "+error;
                return false;
            }
            collectDiagnostics(*c);
            for(auto& bp:c->busPorts) {   // [caws~ name] -> its cable
                if(!bp.first.send)continue;
                const int l=(bp.first.channel-1)*2;
                for(int n=0;n<64;++n){c->busLr[(size_t)n*2]=c->cableOut[(size_t)n*PdBackend::kCableOutChannels+l];c->busLr[(size_t)n*2+1]=c->cableOut[(size_t)n*PdBackend::kCableOutChannels+l+1];}
                cawbus::send(*bp.second,c->busLr.data(),64);
            }
            c->pd->drainOutputs([&](const std::string& kind,const std::string& name,float value){
                if(outputQueue)outputQueue->push(c->event.clipId,c->event.trackId,c->timelineFrame,kind,name,value);
            });
            if(outputQueue)outputQueue->addDropped(c->pd->takeDroppedOutputs());
            float clipPeak=0.f;
            for(int ch=0;ch<std::min(Meter::CH,c->channels);++ch)
                for(int n=0;n<64;++n)clipPeak=std::max(clipPeak,std::fabs(out[n*c->channels+ch]));
            if(c->active) {
                if(clipPeak>1e-5f)c->silentFrames=0;
                else c->silentFrames+=64;
                if(!c->silenceNoticeSent&&c->silentFrames>=static_cast<std::uint64_t>(std::max(1,c->sampleRate)*1.5)) {
                    c->silenceNoticeSent=true;
                    runtimeMessages.push_back("PD clip "+std::to_string(c->event.clipId)+
                        ": warning: no audio detected for 1.5 seconds while this patch is active. Check its output path (dac~) and gain; intentional silence is OK.");
                }
            }
            {   // the track fader x volume lane (after the silence check: a fader at 0 never warns "no audio")
                const float g=trackGains().get(c->event.trackId);
                if(g!=1.f)for(int n=0;n<64*c->channels;++n)out[n]*=g;
            }
            if(c->meter) {
                for(int ch=0;ch<std::min(Meter::CH,c->channels);++ch) {
                    float peak=0;
                    for(int n=0;n<64;++n)peak=std::max(peak,std::fabs(out[n*c->channels+ch]));
                    float old=c->meter->peak[ch].load(std::memory_order_relaxed);
                    while(old<peak && !c->meter->peak[ch].compare_exchange_weak(old,peak,std::memory_order_relaxed)){}
                }
            }
            return true;
        }
        void stop(int id) override {
            auto* c=find(id);if(!c)return;
            c->active=false;
            for(int n=0;n<128;++n)if(c->notes[n]){std::string ignored;c->pd->noteOn(0,n,0,ignored);c->notes[n]=false;}
            number(*c,"caw-gate",0.f);bang(*c,"caw-stop");
            collectDiagnostics(*c);
        }
        // Live coding. Called on the worker when a new plan arrives while the engine
        // runs: a clip whose patch file was re-saved (or replaced) is reloaded into
        // its running libpd instance, new x-field rules are parsed, and the clip
        // gets its start / run messages again so the new patch is initialised like
        // a patch that had just started. Failures are reported in notes and the
        // previous patch keeps playing.
        void applyPlan(const std::vector<pd_timeline::Event>& next,int rate,std::vector<std::string>& notes) {
            for(auto& c:clips) {
                const auto* e=pd_timeline::find(next,c.event.clipId);
                if(!e)continue;
                const bool patchNew=pd_timeline::patchChanged(c.event,*e);
                if(e->pFieldRules!=c.event.pFieldRules) {
                    std::vector<pd_pfield_numeric::Rule> rules;std::string why;
                    if(pd_pfield_numeric::parse(e->pFieldRules,rules,why))c.rules=std::move(rules);
                    else notes.push_back("PD clip "+std::to_string(c.event.clipId)+": warning: x-fields: "+why);
                }
                c.event=*e;
                if(!patchNew||!c.pd)continue;
                std::string why;
                if(!c.pd->reloadPatch(e->patch,e->outputs,why) || !c.pd->openBridge(pd_cables::prepare(e->patch,e->clipId,e->buses),why)) {
                    notes.push_back("PD clip "+std::to_string(c.event.clipId)+": warning: reload failed, the previous patch keeps playing: "+why);
                    continue;
                }
                c.busPorts.clear();   // the cable names may have changed with the patch
                notes.push_back("PD patch reloaded: "+std::filesystem::u8path(e->patch).filename().u8string());
                if(!c.active)continue;
                start(c.event.clipId);
                std::int64_t s=0,en=0;
                if(pd_plan_update::toFrame(e->start,rate,s)&&pd_plan_update::toFrame(e->end,rate,en)&&en>s&&e->runs>=1) {
                    const auto span=en-s;
                    for(int r=e->runs-1;r>=0;--r) {
                        const auto at=s+(span/e->runs)*r+(span%e->runs)*r/e->runs;
                        if(at<=c.timelineFrame){control({at,c.event.clipId,1,r,0});break;}
                    }
                }
            }
        }
        bool control(const pd_frame_scheduler::Control& ctrl) override {
            Clip* c=find(ctrl.id);if(!c){error="PD clip "+std::to_string(ctrl.id)+": unknown clip instance.";return false;}
            if(ctrl.kind==1) {
                number(*c,"caw-p4",c->event.midiEnabled?60.f:0.f);
                number(*c,"caw-p5",c->event.midiEnabled?100.f:0.f);
                for(const auto& rule:c->rules) {
                    double v=0;
                    if(!pd_pfield_numeric::value(rule,ctrl.a,c->event.runs,v)){
                        error="PD clip "+std::to_string(c->event.clipId)+": invalid p-field result.";return false;
                    }
                    number(*c,"caw-p"+std::to_string(rule.field),static_cast<float>(v));
                }
                number(*c,"caw-run",static_cast<float>(ctrl.a));bang(*c,"caw-trigger");
            } else if(ctrl.kind==2) {
                number(*c,"caw-p4",static_cast<float>(ctrl.a));number(*c,"caw-p5",static_cast<float>(ctrl.b));
                if(!c->pd->noteOn(0,ctrl.a,ctrl.b,error)){
                    error="PD clip "+std::to_string(c->event.clipId)+": "+error;return false;
                }
                c->notes[ctrl.a]=true;
            } else if(ctrl.kind==3) {
                if(!c->pd->noteOn(0,ctrl.a,0,error)){
                    error="PD clip "+std::to_string(c->event.clipId)+": "+error;return false;
                }
                c->notes[ctrl.a]=false;
            }
            return true;
        }
    };
private:
    struct Command {std::int64_t frame=0,loopStart=0,loopEnd=0;bool playing=false,loop=false;};
public:
    struct TrackPeak {int trackId;std::array<float,Meter::CH> value;};
    PdDawEngine()=default;
    ~PdDawEngine(){stop();}
    PdDawEngine(const PdDawEngine&)=delete;
    PdDawEngine& operator=(const PdDawEngine&)=delete;
    bool start(const std::string& library,const std::vector<pd_timeline::Event>& plan,const CawAudioSettings& settings) {
        if(active_.load())return false;
        if(worker_.joinable())worker_.join();
        {std::lock_guard<std::mutex> lock(mutex_);command_={};status_="Preloading PD clips...";}
        {std::lock_guard<std::mutex> lock(mutex_);pendingPlan_.clear();planError_.clear();}
        planGeneration_.store(0);
        std::atomic_store(&meters_,std::shared_ptr<std::vector<std::shared_ptr<Meter>>>{});
        std::atomic_store(&capture_,std::shared_ptr<MonitorCapture>{});
        std::atomic_store(&controlQueue_,std::shared_ptr<PdControlQueue>{});
        generation_.store(1);stop_.store(false);ready_.store(false);active_.store(true);position_.store(-1);underflows_.store(0);
        try{worker_=std::thread(&PdDawEngine::run,this,library,plan,settings);}
        catch(...){active_.store(false);setStatus("Could not start PD worker.");return false;}
        return true;
    }
    void transport(bool playing,double time,double loopStart,double loopEnd,bool loop,int rate){
        auto frames=[rate](double v){return static_cast<std::int64_t>(std::llround(std::max(0.0,v)*rate));};
        Command c;c.frame=frames(time);c.playing=playing;c.loop=loop;c.loopStart=frames(loopStart);c.loopEnd=frames(loopEnd);
        {std::lock_guard<std::mutex> lock(mutex_);command_=c;}
        generation_.fetch_add(1,std::memory_order_release);
        wake_.notify_all();
    }
    bool replan(const std::vector<pd_timeline::Event>& plan) {
        if(!ready())return false;
        {std::lock_guard<std::mutex> lock(mutex_);
        pendingPlan_=plan;planGeneration_.fetch_add(1,std::memory_order_release);}
        wake_.notify_all();return true;
    }
    std::string takePlanError() {
        std::lock_guard<std::mutex> lock(mutex_);std::string result;result.swap(planError_);return result;
    }
    // Messages of live patch reloads (successes and non-fatal failures).
    std::vector<std::string> takeNotes() {
        std::lock_guard<std::mutex> lock(mutex_);std::vector<std::string> result;result.swap(notes_);return result;
    }
    std::vector<PdControlOutput> takeControlOutputs() {
        auto queue=std::atomic_load(&controlQueue_);return queue?queue->take():std::vector<PdControlOutput>{};
    }
    unsigned takeDroppedControls() {auto q=std::atomic_load(&controlQueue_);return q?q->takeDropped():0;}
    void stop(){stop_.store(true);wake_.notify_all();if(worker_.joinable())worker_.join();}
    bool active()const{return active_.load();}
    // CAW hub (caws/cawr): set before start; the worker delivers bus values to the instances.
    void setHub(CawHub* hub){hub_=hub;}
    bool ready()const{return ready_.load();}
    std::int64_t position()const{return position_.load();}
    std::uint64_t underflows()const{return underflows_.load();}
    std::string status()const{std::lock_guard<std::mutex> lock(mutex_);return status_;}
    bool takeSnapshot(MonitorCapture::Snapshot& out, std::uint64_t lastSequence) const {
        auto capture = std::atomic_load(&capture_);
        return capture && capture->take(out, lastSequence);
    }
    std::vector<TrackPeak> takePeaks() {
        std::vector<TrackPeak> result;
        auto meters=std::atomic_load(&meters_);
        if(!meters)return result;
        result.reserve(meters->size());
        for(const auto& m:*meters) {
            TrackPeak tp;tp.trackId=m->trackId;
            for(int ch=0;ch<Meter::CH;++ch)tp.value[(size_t)ch]=m->peak[(size_t)ch].exchange(0.f);
            result.push_back(tp);
        }
        return result;
    }
private:
    void setStatus(const std::string& s){std::lock_guard<std::mutex> lock(mutex_);status_=s;}
    static bool frame(double time,int rate,std::int64_t& result){
        if(!std::isfinite(time)||time<0||time>static_cast<double>(INT64_MAX/rate))return false;
        result=static_cast<std::int64_t>(std::llround(time*rate));return true;
    }
    void run(std::string library,std::vector<pd_timeline::Event> plan,CawAudioSettings settings) noexcept {
        try {
            // The virtual soundcard render width: the soundcard setup's width, widened to the
            // widest [dac~ N] any planned patch writes. The device stream stays at the setup's
            // width - the rendered channels fold into it at the push (k mod N), and the meters
            // see every channel separately.
            int renderCh=std::min(8,std::max(settings.channels,pdPlanMaxChannel(plan)));
            if(renderCh<1)renderCh=1;
            std::string error;PdIsolatedLibraries copies;
            PdBackend emptyEngine;
            if(plan.empty()) {
                if(!emptyEngine.openEngine(library,settings.sampleRate,renderCh,error)){setStatus("PD: "+error);active_.store(false);return;}
            } else if(!copies.create(library,plan.size(),error)){setStatus(error);active_.store(false);return;}
            Sink sink;
            auto controlQueue=std::make_shared<PdControlQueue>();sink.outputQueue=controlQueue.get();
            auto meters=std::make_shared<std::vector<std::shared_ptr<Meter>>>();
            std::vector<pd_frame_scheduler::Event> events;
            std::vector<pd_frame_scheduler::Control> controls;
            sink.clips.reserve(plan.size());
            for(std::size_t i=0;i<plan.size();++i) {
                auto& e=plan[i];std::int64_t start=0,end=0;
                if(!frame(e.start,settings.sampleRate,start)||!frame(e.end,settings.sampleRate,end)||end<=start){
                    setStatus("PD clip "+std::to_string(e.clipId)+": invalid or sub-sample timing.");active_.store(false);return;
                }
                Clip c;c.event=e;c.channels=renderCh;c.sampleRate=settings.sampleRate;
                if(e.trackId>0) {
                    for(const auto& meter:*meters)if(meter->trackId==e.trackId){c.meter=meter;break;}
                    if(!c.meter){c.meter=std::make_shared<Meter>(e.trackId);meters->push_back(c.meter);}
                }
                if(!pd_pfield_numeric::parse(e.pFieldRules,c.rules,error)){
                    setStatus("PD clip "+std::to_string(e.clipId)+": p-fields: "+error);active_.store(false);return;
                }
                c.pd=std::make_unique<PdBackend>();
                if(!c.pd->open(copies.paths()[i],e.patch,settings.sampleRate,renderCh,error)){
                    setStatus("PD clip "+std::to_string(e.clipId)+": "+error);active_.store(false);return;
                }
                if(!c.pd->bindOutputs(e.outputs,error)){
                    setStatus("PD clip "+std::to_string(e.clipId)+": outputs: "+error);active_.store(false);return;
                }
                {   // CAW cables: [cawr~ name] / [caws~ name] -> the bridge patch in the same instance
                    std::string why;
                    if(!c.pd->openBridge(pd_cables::prepare(e.patch,e.clipId,e.buses),why))
                        setStatus("PD clip "+std::to_string(e.clipId)+": "+why);
                }
                sink.collectDiagnostics(c);
                events.push_back({e.clipId,start,end,e.muted});
                for(int r=0;r<e.runs;++r) {
                    const auto span=end-start;
                    const auto at=start+(span/e.runs)*r+(span%e.runs)*r/e.runs;
                    if(at<end)controls.push_back({at,e.clipId,1,r,0});
                }
                if(e.midiEnabled)for(const auto& n:e.notes) {
                    std::int64_t on=0,off=0;
                    if(!frame(n.start,settings.sampleRate,on)||!frame(n.end,settings.sampleRate,off))continue;
                    on=std::max(start,on);off=std::min(end,off);
                    if(on>=end||off<=on)continue;
                    controls.push_back({on,e.clipId,2,n.pitch,n.velocity});
                    if(off<end)controls.push_back({off,e.clipId,3,n.pitch,0});
                }
                sink.clips.push_back(std::move(c));
            }
            {
                auto diagnostics=sink.takeRuntimeMessages();
                if(!diagnostics.empty()) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    for(auto& message:diagnostics)notes_.push_back(std::move(message));
                }
            }
            pd_frame_scheduler::Scheduler scheduler;
            if(!scheduler.prepare(events,renderCh,error,std::move(controls))){setStatus(error);active_.store(false);return;}
            auto capture = std::make_shared<MonitorCapture>(settings.sampleRate, settings.channels);
            CawAudioOutput output;settings.fullScale=1.f;
            output.setMonitorCapture(capture.get());
            if(!output.open(settings,error)){setStatus("PD audio: "+error);active_.store(false);return;}
            output.setExpectingAudio(false);
            if(stop_.load()){active_.store(false);return;}
            std::atomic_store(&meters_,meters);
            std::atomic_store(&controlQueue_,controlQueue);
            std::atomic_store(&capture_,capture);
            setStatus(plan.empty() ? "PD DSP ready." : "PD ready: all clips preloaded.");ready_.store(true);
            std::vector<float> samples(static_cast<std::size_t>(64*renderCh));
            std::vector<float> pdStream;       // the folded push buffer: renderCh channels -> the soundcard's width
            std::uint64_t seen=0,seenPlan=0;Command command;
            auto publishRuntimeMessages=[&] {
                auto messages=sink.takeRuntimeMessages();
                if(messages.empty())return;
                std::lock_guard<std::mutex> lock(mutex_);
                for(auto& message:messages)notes_.push_back(std::move(message));
            };
            while(!stop_.load()) {
                if(planGeneration_.load(std::memory_order_acquire)!=seenPlan) {
                    std::vector<pd_timeline::Event> next;
                    {std::lock_guard<std::mutex> lock(mutex_);next=pendingPlan_;seenPlan=planGeneration_.load(std::memory_order_relaxed);}
                    std::vector<pd_frame_scheduler::Event> newEvents;
                    std::vector<pd_frame_scheduler::Control> newControls;
                    std::string why;
                    if(!pd_timeline::sameInstances(plan,next))why="The set of PD clips changed; restart the engine.";
                    else if(pd_plan_update::build(next,settings.sampleRate,newEvents,newControls,why) &&
                            scheduler.replan(newEvents,std::move(newControls),command.playing,why)) {
                        auto updated=std::make_shared<std::vector<std::shared_ptr<Meter>>>(*meters);
                        std::vector<std::string> liveNotes;
                        sink.applyPlan(next,settings.sampleRate,liveNotes);
                        if(!liveNotes.empty()){std::lock_guard<std::mutex> lock(mutex_);for(auto& n:liveNotes)notes_.push_back(std::move(n));}
                        for(auto& clip:sink.clips) {
                            const auto* e=pd_timeline::find(next,clip.event.clipId);
                            clip.meter.reset();
                            if(e->trackId>0) {
                                for(const auto& m:*updated)if(m->trackId==e->trackId){clip.meter=m;break;}
                                if(!clip.meter){clip.meter=std::make_shared<Meter>(e->trackId);updated->push_back(clip.meter);}
                            }
                        }
                        meters=updated;std::atomic_store(&meters_,meters);plan=std::move(next);
                    }
                    if(!why.empty()){std::lock_guard<std::mutex> lock(mutex_);planError_=why;}
                }
                const auto gen=generation_.load(std::memory_order_acquire);
                if(gen!=seen) {
                    {std::lock_guard<std::mutex> lock(mutex_);command=command_;}
                    if(command.loop && command.loopEnd>command.loopStart &&
                       (command.frame<command.loopStart || command.frame>=command.loopEnd))
                        command.frame=command.loopStart;
                    if(!output.discardPending(stop_))break;
                    output.setExpectingAudio(command.playing);
                    scheduler.seek(command.frame,sink);
                    output.clearPlayedPosition();position_.store(command.frame);
                    seen=gen;
                    continue;
                }
                if(!command.playing){
                    // Parked while the transport is stopped: no polling, no DSP. transport(),
                    // replan() and stop() wake this thread.
                    std::unique_lock<std::mutex> parked(wakeMutex_);
                    wake_.wait_for(parked,std::chrono::milliseconds(250),[&]{
                        return stop_.load()||generation_.load(std::memory_order_acquire)!=seen||
                               planGeneration_.load(std::memory_order_acquire)!=seenPlan;
                    });
                    continue;
                }
                auto begin=scheduler.frame();
                if(command.loop && command.loopEnd>command.loopStart && begin>=command.loopEnd){
                    scheduler.seek(command.loopStart,sink);begin=scheduler.frame();
                }
                unsigned count=64;
                if(command.loop && command.loopEnd>command.loopStart)
                    count=static_cast<unsigned>(std::min<std::int64_t>(64,command.loopEnd-begin));
                if(!count)continue;
                if(!scheduler.render(samples.data(),count,sink)){
                    publishRuntimeMessages();
                    setStatus("PD processing: "+sink.error);break;
                }
                publishRuntimeMessages();
                // CAW hub (caws/cawr): push shared bus values into every running PD
                // clip instance. A patch without [r $0-cawr-<name>] ignores them.
                if(hub_) {
                    std::map<std::string,double> hubValues;
                    if(hub_->take(hubSeen_,hubValues)) {
                        std::string why;
                        for(auto& clip:sink.clips)if(clip.pd)
                            for(const auto& entry:hubValues)
                                clip.pd->numberLocal("cawr-"+entry.first,static_cast<float>(entry.second),why);
                    }
                }
                for(unsigned i=0;i<count*static_cast<unsigned>(renderCh);++i)
                    samples[i]=std::max(-1.f,std::min(1.f,samples[i]));
                {   // The VST rack's PureData inputs (engine 1) hear the live mix through the shared
                    // ring - the dedicated PD engine renders into its OWN device, which the rack
                    // cannot read. The ring stores every rendered channel; the rack folds or takes
                    // them 1:1 per its receive mode.
                    pdlivering::ring.pushStride2(samples.data(),count,renderCh);
                }
                {
                    // The virtual soundcard fold: the patch may render more channels than the
                    // soundcard setup - channel k plays on stream channel k mod N.
                    const unsigned streamCh=static_cast<unsigned>(std::max(1,settings.channels));
                    if(renderCh==static_cast<int>(streamCh)) {
                        if(!output.pushTagged(samples.data(),count,stop_,begin,&generation_,seen)){
                            if(!stop_.load() && generation_.load()==seen){setStatus("PD audio output stopped.");break;}
                            continue;
                        }
                    } else {
                        if(pdStream.size()<(size_t)count*streamCh)pdStream.resize((size_t)count*streamCh,0.f);
                        std::fill_n(pdStream.begin(),(size_t)count*streamCh,0.f);
                        for(unsigned i=0;i<count;++i) {
                            const float* in=samples.data()+(size_t)i*renderCh;
                            float* dst=pdStream.data()+(size_t)i*streamCh;
                            for(int c=0;c<renderCh;++c)dst[c%streamCh]+=in[c];
                        }
                        if(!output.pushTagged(pdStream.data(),count,stop_,begin,&generation_,seen)){
                            if(!stop_.load() && generation_.load()==seen){setStatus("PD audio output stopped.");break;}
                            continue;
                        }
                    }
                }
                const auto played=output.playedPosition();
                if(played>=0)position_.store(played);
                underflows_.store(output.underflows());
            }
            scheduler.seek(scheduler.frame(),sink);
            output.close();
            if(stop_.load())setStatus("PD stopped.");
        }catch(const std::exception& e){setStatus(std::string("PD error: ")+e.what());}
        catch(...){setStatus("PD failed unexpectedly.");}
        ready_.store(false);active_.store(false);
    }
    std::thread worker_;
    std::mutex wakeMutex_;            // the worker parks on this while the transport is stopped
    std::condition_variable wake_;
    std::atomic<bool> stop_{false},active_{false},ready_{false};
    std::atomic<std::uint64_t> generation_{0},underflows_{0};
    std::atomic<std::uint64_t> planGeneration_{0};
    std::atomic<std::int64_t> position_{-1};
    mutable std::mutex mutex_;Command command_;std::string status_="PD off.";
    CawHub* hub_=nullptr;std::uint64_t hubSeen_=0;
    std::vector<pd_timeline::Event> pendingPlan_;
    std::string planError_;
    std::vector<std::string> notes_;
    std::shared_ptr<std::vector<std::shared_ptr<Meter>>> meters_;
    std::shared_ptr<MonitorCapture> capture_;
    std::shared_ptr<PdControlQueue> controlQueue_;
};
