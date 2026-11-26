#pragma once
#include "model.h"
#include "pd_control_syntax.h"
#include "pd_patch_editor.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

// PD timeline plan. Each clip is assigned an independent preloaded runtime.
// Paths reference external .pd files; project moves may need relinking.
namespace pd_timeline {
// stamp = last-modified time of the patch file, so saving the patch in an
// editor is noticed while the timeline plays.
struct Note { double start=0, end=0; int pitch=60, velocity=100; };
struct Automation {std::string name; double fallback=.5; bool enabled=true; std::vector<AutoPoint> points;};
struct Event {
    int clipId; double start; double end; std::string patch; long long stamp = 0;
    int runs=1; std::string pFieldRules; bool midiEnabled=false; std::vector<Note> notes;
    int trackId=0;
    std::vector<Automation> automation;
    std::vector<pd_control_syntax::Output> outputs;
    std::vector<pd_control_syntax::BusLink> buses;   // CAW AUDIO BUSES: cawbrecv / cawbsend comment lines of the patch
    // Muted by the track / clip M / S state. The clip stays loaded (its instance is
    // preloaded with the rest), it is simply not ticked - so muting never restarts
    // the engine or stops the transport while the other engines keep playing.
    bool muted = false;
};
// otherEngineRunning: clips of Csound/VST/SC tracks are only a conflict while
// that engine really runs. Without a running engine they are silent anyway, so
// they are skipped and the PD clips still play. badClip receives the id of the
// clip an error is about (0 = none), so the app can open it.
// includeMuted: include the clips that M / S silences and mark them `muted` instead of
// dropping them. The timeline engine loads every clip that way, so a mute is applied
// live by the scheduler - the engine, its device stream and the transport never restart.
inline bool makePlan(const Project& p, const std::string& projectPath,
                     std::vector<Event>& events, std::string& error,
                     bool otherEngineRunning = true, int* badClip = nullptr,
                     bool includeMuted = false) {
    events.clear(); error.clear(); if(badClip)*badClip=0;
    bool anySolo=false; for (const auto& t:p.tracks) if(t.solo && !t.muted) anySolo=true;
    bool otherAudio=false;
    bool audiblePd=false;   // a PD clip that can actually sound: muted PD clips are not a conflict
    for(const auto& t:p.tracks) {
        const bool trackAudible = !t.muted && !(anySolo && !t.solo) && !trackFrozen(t);   // frozen: the PD clips are baked into the WAV
        if(!trackAudible && !includeMuted) continue;
        for(const auto& c:t.clips) {
            const bool clipAudible = trackAudible && !c.muted;
            if(!clipAudible && !includeMuted) continue;
            if(t.kind==6) {
                if(c.id<=0 || !std::isfinite(c.start) || !std::isfinite(c.dur) || c.start<0 || c.dur<=0 || !std::isfinite(c.start+c.dur)) {
                    error="A PD clip has invalid timing or identity.";if(badClip)*badClip=c.id;return false;
                }
                if(c.code.empty()) continue; // an empty clip does not prevent switching PD DSP on
                std::filesystem::path file=std::filesystem::u8path(c.code);
                if(file.is_relative()) {
                    if(projectPath.empty()){error="A relative PD patch path requires a saved project.";return false;}
                    file=std::filesystem::u8path(projectPath).parent_path()/file;
                }
                std::error_code ec;
                file=std::filesystem::absolute(file,ec);
                if(ec || !std::filesystem::is_regular_file(file,ec) || ec || file.extension()!=".pd") {
                    error="PD patch for clip '"+c.name+"' is missing or not a .pd file.";if(badClip)*badClip=c.id;return false;
                }
                const auto written=std::filesystem::last_write_time(file,ec);
                Event ev{c.id,c.start,c.start+c.dur,file.u8string(),ec?0LL:(long long)written.time_since_epoch().count()};
                ev.runs=std::max(1,std::min(256,c.runs)); ev.pFieldRules=c.pFieldRules; ev.midiEnabled=c.midiEnabled;ev.trackId=t.id;
                ev.muted=!clipAudible;
                std::string source,readError;
                if(!pd_patch_editor::read(file,source,readError)) {
                    error="Cannot read PD patch for clip '"+c.name+"': "+readError;if(badClip)*badClip=c.id;return false;
                }
                ev.outputs=pd_control_syntax::outputs(source);
                ev.buses=pd_control_syntax::busLinks(source);
                for(const auto& name:pd_control_syntax::inputs(source)) {
                    Automation a;a.name=name;
                    auto range=t.automationRanges.find(name);
                    if(range!=t.automationRanges.end() && std::isfinite(range->second.value))a.fallback=range->second.value;
                    a.enabled=!t.scoreAutoMuted.count(name) && (t.scoreAutoSolo.empty()||t.scoreAutoSolo.count(name));
                    const auto lane=t.automation.find(name);
                    if(lane!=t.automation.end())for(const auto& point:lane->second)
                        if(std::isfinite(point.t)&&std::isfinite(point.v))a.points.push_back(point);
                    std::stable_sort(a.points.begin(),a.points.end(),[](const AutoPoint& x,const AutoPoint& y){return x.t<y.t;});
                    ev.automation.push_back(std::move(a));
                }
                for(const auto& mc:t.midiClips) if(!mc.muted && mc.clipId==c.id)
                    for(const auto& n:mc.notes) {
                        if(!std::isfinite(n.start)||!std::isfinite(n.dur)||n.start<0||n.dur<=0||n.start>=mc.dur)continue;
                        const double start=std::max(c.start,mc.start+n.start);
                        const double end=std::min(c.start+c.dur,std::min(mc.start+mc.dur,mc.start+n.start+n.dur));
                        if(end>start)ev.notes.push_back({start,end,std::max(0,std::min(127,n.pitch)),std::max(1,std::min(127,n.velocity))});
                    }
                events.push_back(std::move(ev));
                if(!events.back().muted)audiblePd=true;
            } else if(trackAudible && (t.kind==0 || t.kind==5)) otherAudio=true;
            // VST tracks (kind 3 / 4) are NOT other audio: the instruments render in CAW's VST
            // engine and the effects in the Csound rack, both mixed into the same output stream,
            // so PD and VST tracks play together like tracks inside one DAW.
        }
    }
    // Only an audible PD clip conflicts. Muting every PD clip (or soloing another
    // track) leaves the loaded instances in place and silent - it must not fail the
    // plan, which would stop the PD timeline instead of just silencing it.
    if(audiblePd && otherAudio && otherEngineRunning) {error="Mixed PD and Csound/VST/SC clips are not supported yet; mute the other tracks.";return false;}
    std::sort(events.begin(),events.end(),[](const auto& a,const auto& b){return a.start<b.start;});
    return true;
}
inline const Event* find(const std::vector<Event>& events,int clipId) {
    for(const auto& e:events)if(e.clipId==clipId)return &e;
    return nullptr;
}
inline bool samePlan(const std::vector<Event>& a,const std::vector<Event>& b){
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i){
        const auto& x=a[i];const auto& y=b[i];
        if(x.clipId!=y.clipId||x.start!=y.start||x.end!=y.end||x.patch!=y.patch||x.stamp!=y.stamp||
           x.runs!=y.runs||x.pFieldRules!=y.pFieldRules||x.midiEnabled!=y.midiEnabled||x.trackId!=y.trackId||
           x.muted!=y.muted||x.notes.size()!=y.notes.size()||x.automation.size()!=y.automation.size())return false;
        for(std::size_t n=0;n<x.notes.size();++n)
            if(x.notes[n].start!=y.notes[n].start||x.notes[n].end!=y.notes[n].end||
               x.notes[n].pitch!=y.notes[n].pitch||x.notes[n].velocity!=y.notes[n].velocity)return false;
        for(std::size_t k=0;k<x.automation.size();++k){
            const auto& a=x.automation[k];const auto& b=y.automation[k];
            if(a.name!=b.name||a.fallback!=b.fallback||a.enabled!=b.enabled||a.points.size()!=b.points.size())return false;
            for(std::size_t j=0;j<a.points.size();++j)
                if(a.points[j].t!=b.points[j].t||a.points[j].v!=b.points[j].v||a.points[j].line!=b.points[j].line)return false;
        }
    }
    return true;
}
// The loaded instances (one isolated libpd per clip) can be kept when the same
// clips are still planned. Timing, MIDI notes, runs, x-fields and even the
// patch itself (re-saved or another file) are applied live to a running engine,
// so only a different SET of clips needs a restart.
inline bool sameInstances(const std::vector<Event>& a,const std::vector<Event>& b) {
    if(a.size()!=b.size())return false;
    for(const auto& x:a)if(!find(b,x.clipId))return false;
    return true;
}
// A sounding clip must be restarted only when the patch it plays changed
// (another file was chosen or the file was saved). Moving, trimming or
// muting never restarts it: the next events simply replace the old ones.
inline bool patchChanged(const Event& playing,const Event& next) {
    return playing.patch!=next.patch || playing.stamp!=next.stamp;
}
inline const Event* at(const std::vector<Event>& events,double time) {
    for(const auto& e:events)if(time>=e.start && time<e.end)return &e;
    return nullptr;
}
// Clamp a UI-frame advance to the first boundary. Otherwise a short clip
// can be skipped entirely when a frame takes longer than the clip.
inline double firstBoundary(const std::vector<Event>& events, double from, double to) {
    if (to <= from) return to;
    double result = to;
    for (const auto& e : events) {
        if (e.start > from && e.start < result) result = e.start;
        if (e.end > from && e.end < result) result = e.end;
    }
    return result;
}
}
