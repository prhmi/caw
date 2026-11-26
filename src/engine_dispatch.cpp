#include "app_shared.h"
#include "caw_audio_bus.h"
#include <fstream>
#include "caw_audio_in.h"

// src/engine_dispatch.cpp (phase 2: extracted from app.cpp; the shared state is app_state.h, the
//  internal declarations are app_internal.h - all included through app_shared.h.)

// TRACK FREEZE (Cubase-style lock): the render pass state machine + the frozen-track playback.
#include "track_freeze.h"
#include "undo_files.h"
#include "rack_offline.h"
#include "pd_live_ring.h"
#include "sc_nrt.h"

// An OFFLINE render (freeze / export part) must start its plugin exactly where the live sound is.
// The saved caches (parameter state + patch blob) can be stale: a patch edited inside the plugin
// window that the state poll never re-captured, or a project-saved blob from an older session,
// and the offline plugin then plays a DIFFERENT patch than the live instance (the live instance
// is reused and carries every change since it was created). So the offline jobs read the LIVE
// engine instance's CURRENT patch state and parameter values directly, with the render paused.
static void captureLiveVstInstanceState(std::vector<VstJob>& jobs) {
#ifdef CAW_VST3
    for (VstJob& j : jobs) {
        // Every chain stage refreshes its own state. Stage 0 = the track's plugin; stages 1..7
        // live under the synthetic key id*8+stage in the editor slots and under the same key in
        // the engine rack. The LIVE ENGINE instance wins when Run VST is on; otherwise the
        // EDITOR instance (the plugin window's own instance - it always exists once the plugin
        // is picked and holds exactly what the user sees / tweaked in the GUI). Without this
        // fallback an export with the VST engine off fell back to the saved caches and could
        // load a stale patch.
        const int stageCount = 1 + (int)j.chain.size();
        for (int st = 0; st < stageCount; ++st) {
            std::shared_ptr<caw::IPlugin> plug;
            bool viaEngine = false;
            if (g_vstEngineOn) {
                plug = st == 0 ? vstSolo().pluginOfTrack(j.id) : vstSolo().pluginOfTrackStage(j.id, st);
                viaEngine = plug != nullptr;
            }
            if (!plug) {
                const auto it = vstui::slots().find(st == 0 ? j.id : j.id * 8 + st);
                if (it != vstui::slots().end()) plug = it->second.plugin;
            }
            if (!plug) continue;
            auto read = [&] {
                std::string blob = plug->getState();          // the instance's CURRENT patch state
                if (!blob.empty()) {
                    if (st == 0) j.stateBlob = blob;
                    else j.chain[(size_t)st - 1].stateBlob = blob;
                }
                std::vector<double> vals;
                const int n = plug->readParameterValues(vals);
                const auto& ps = plug->params();
                std::map<uint32_t, double>* stateMap = st == 0 ? &j.parameterState : &j.chain[(size_t)st - 1].parameterState;
                if (n > 0 && (size_t)n == ps.size())
                    for (const auto& p : ps)
                        if (p.index >= 0 && (size_t)p.index < vals.size()) (*stateMap)[p.id] = vals[(size_t)p.index];
            };
            if (viaEngine) {   // off the render thread: pause it briefly, retry while it stays busy
                bool ok = false;
                for (int attempt = 0; attempt < 20 && !ok; ++attempt)
                    ok = vstSolo().withRenderPaused(read);
                if (!ok) read();   // the engine stays busy: the API mutex still serializes the call
            } else {
                read();
            }
        }
    }
#else
    (void)jobs;
#endif
}

bool scManualRunning(int clipId) {
    auto it = g_scManualRuns.find(clipId);
    if (it == g_scManualRuns.end()) return false;
    if (it->second <= std::chrono::steady_clock::now()) { g_scManualRuns.erase(it); return false; }
    return true;
}
#include "app.h"
CawScRing* sampleRingOf(int trackId) {
    std::lock_guard<std::mutex> lock(g_sampleRingMutex);
    auto& r = g_sampleRings[trackId];
    if (!r) r = std::make_unique<CawScRing>();
    return r.get();
}
#include "app.h"
CawScRing* scTrackRingOf(int slot) {
    if (slot < 0 || slot >= ScEngine::maxTrackSlots) return nullptr;
    std::lock_guard<std::mutex> lock(g_scTrackRingMutex);
    auto& r = g_scTrackRings[slot];
    if (!r) r = std::make_unique<CawScRing>();
    return r.get();
}
#include "app.h"
void updateScSlotSuppress() {   // which SuperCollider track slots an FX-only VSTe consumes: their raw mix no longer goes to the master
    std::uint32_t mask = 0;
    bool anySolo = false;
    for (const Track& t : g_proj.tracks) if (t.solo && !t.muted) anySolo = true;
    for (const Track& vt : g_proj.tracks) {
        if (vt.kind != 4 || vt.vstPath.empty() || vt.vstOutMode != 0) continue;
        if (anySolo && !vt.solo) continue;   // the VSTe muted by another track's solo: the solo rule already silences the consumed source, and a soloed source keeps its raw sound
        for (const auto& in : vt.vstInputs) {
            if (!in.on || in.mode == 2) continue;
            for (size_t i = 0; i < g_proj.tracks.size(); ++i) {
                const Track& o = g_proj.tracks[i];
                if (o.kind != 5 || trackFrozen(o) || o.muted) continue;
                const bool match = in.mode == 0 ? o.name == in.name
                    : std::any_of(o.clips.begin(), o.clips.end(), [&](const Clip& c) { return c.name == in.name; });
                if (!match) continue;
                const int slot = scTrackSlot(o.id);
                if (slot >= 0 && slot < ScEngine::kFirstBusSlot) mask |= (1u << slot);
            }
        }
    }
    g_scEngine.setSlotSuppress(mask);
}
#include "app.h"
void clearSampleRings() {   // drops the stale ring audio (a late-starting consumer would stay behind forever)
    std::lock_guard<std::mutex> lock(g_sampleRingMutex);
    for (auto& kv : g_sampleRings) if (kv.second) kv.second->reset();
}
#include "app.h"
void startEngine();
#include "app.h"
void pdTransport(bool play);
#include "app.h"
void stopScEngine();
#include "app.h"
void startRender();
#include "app.h"
void cancelRender();
#include "app.h"
bool replaceOutputFile(const std::string& temporary, const std::string& final, std::string& error);
#include "app.h"
cawaudio::AudioHandle audioClipData(const Clip& c, std::string& error);
#include "app.h"
void audioKillClipVoices(int clipId);   // stop the running audio voice of a clip (defined with the audio-track code)
#include "app.h"
void closeSampleOutput();               // the sample engine's own output stream (Run sample alone)
#include "app.h"
void ensureSampleOutput();              // open the sample stream when nothing else owns the card
#include "app.h"
void handAudioCardToNextStream();       // close CAW's own streams: the next stream to open becomes the carrier
#include "app.h"
std::array<int, 8> audioClipOutMap(const Clip& c, const cawaudio::AudioHandle& data);   // per output: which input channel plays (-1 = silent)
#include "app.h"
cawaudio::AudioHandle audioClipData(const Clip& c, std::string& error);
#include "app.h"
void audioKillClipVoices(int clipId);   // stop the running audio voice of a clip (defined with the audio-track code)
#include "app.h"
std::shared_ptr<const vstscript::Program> sampleScript(const Clip& c) {
    auto it = g_sampleScripts.find(c.id);
    if (it != g_sampleScripts.end() && it->second) return it->second;
    auto prog = std::make_shared<const vstscript::Program>(vstscript::Compiler().compile(c.code));
    logScriptError("sample clip", c.id, c.name, *prog);
    g_sampleScripts[c.id] = prog;
    return prog;
}
#include "app.h"
PdUiControls pdControls(const Clip& clip) {
    if(clip.code.empty())return {};
    auto file=std::filesystem::u8path(clip.code);
    if(file.is_relative()) {
        if(g_projectPath.empty())return {};
        file=std::filesystem::u8path(g_projectPath).parent_path()/file;
    }
    std::error_code ec;
    file=std::filesystem::absolute(file,ec).lexically_normal();
    if(ec)return {};
    const auto stamp=std::filesystem::last_write_time(file,ec);
    if(ec)return {};
    struct Entry {std::filesystem::file_time_type stamp;PdUiControls controls;};
    static std::map<std::string,Entry> cache;
    const auto key=file.u8string();
    auto found=cache.find(key);
    if(found!=cache.end()&&found->second.stamp==stamp)return found->second.controls;
    std::string source,error;
    if(!pd_patch_editor::read(file,source,error))return {};
    Entry entry{stamp,{pd_control_syntax::inputs(source),pd_control_syntax::outputs(source)}};
    if(cache.size()>256)cache.clear();
    cache[key]=entry;return entry.controls;
}
#include "app.h"
void recFeed(int idx, float v) {
    if (!g_playing || !g_timelineRun || g_recArm.empty() || idx < 0 || idx >= (int)g_recNames.size() || !std::isfinite(v)) return;
    const CawWriteTarget& target = g_recNames[(size_t)idx];
    for (Track& t : g_proj.tracks)
        for (Clip& c : t.clips) {
            if (c.name != target.clip || g_play < c.start || g_play > c.start + c.dur + 0.05) continue;
            for (const auto& arm : g_recArm) {
                if (arm.first != t.id || (!target.channel.empty() && arm.second != target.channel)) continue;
                RecBuf* buf = nullptr;
                for (RecBuf& b : g_recBufs) if (b.trackId == arm.first && b.channel == arm.second) { buf = &b; break; }
                if (!buf) { g_recBufs.push_back(RecBuf()); buf = &g_recBufs.back(); buf->trackId = arm.first; buf->channel = arm.second; }
                if (!buf->s.empty() && g_play <= buf->s.back().t) continue;
                AutoPoint p; p.t = g_play; p.v = v;
                buf->s.push_back(p);
            }
        }
}
#include "app.h"
void drainPdControlOutputs() {
    if(!g_mixedPd && !g_pdEngineOn)return;
    auto values=g_mixedPd?g_apiProc.takePdControlOutputs():g_pdTimelineEngine.takeControlOutputs();
    const unsigned dropped=g_mixedPd?g_apiProc.takePdDroppedControls():g_pdTimelineEngine.takeDroppedControls();
    if(dropped)logLine("[caw] PD control queue dropped "+std::to_string(dropped)+" messages.");
    if(!g_timelineRun)return;
    for(const auto& value:values) {
        if(!std::isfinite(value.value))continue;
        if(value.kind==3) { g_cawHub.send(value.name, value.value); continue; }   // caws: PD -> shared hub
        Track* track=findTrackById(value.trackId);
        if(!track)continue;
        Clip* clip=nullptr;for(auto& c:track->clips)if(c.id==value.clipId){clip=&c;break;}
        if(!clip)continue;
        const double time=static_cast<double>(value.frame)/std::max(1000,g_proj.cs.sr);
        const std::string name=value.name;
        if(value.kind==1 && g_recArm.count({track->id,name})) {
            RecBuf* buf=nullptr;
            for(auto& b:g_recBufs)if(b.trackId==track->id&&b.channel==name){buf=&b;break;}
            if(!buf){g_recBufs.push_back(RecBuf());buf=&g_recBufs.back();buf->trackId=track->id;buf->channel=name;}
            if(buf->s.size()<100000 && (buf->s.empty()||time>buf->s.back().t))
                buf->s.push_back({time,value.value});
        } else if(value.kind==2) {
            auto& samples=g_pdScoreData[clip->name].data;
            bool accept=samples.size()<100000;
            for(auto it=samples.rbegin();accept&&it!=samples.rend();++it)if(it->name==name){
                accept=time>=it->t+.05;break;
            }
            if(accept){ScoreDataSample sample;sample.t=time;sample.v=value.value;sample.name=name;sample.rate=1;samples.push_back(std::move(sample));}
        }
    }
}
#include "app.h"
void stopPlayback();
#include "app.h"
void stopScEngine();
#include "app.h"
void closeScSoloOutput();
#include "app.h"
void restoreScSoloOutput();
#include "app.h"
void ensureVstSoloOutput();
#include "app.h"
#include "app.h"
void startVstEngine();
#include "app.h"
void vstEngineCheckJobs(float dt);
#include "app.h"
bool engineIsUp();
#include "app.h"
std::map<int, unsigned> collectVstInputMasks(const Project& project);
#include "app.h"
std::set<int> collectVstActiveTracks(const Project& project);
#include "app.h"
CawAudioSettings pdAudioSettings() {
    CawAudioSettings audio;
    audio.sampleRate = g_proj.cs.sr;
    audio.channels = masterChannels(g_proj.cs);   // the project's output channels (out1..outN) - the device clamps to what it has
    audio.softwareBuffer = g_proj.cs.bufSW; audio.hardwareBuffer = g_proj.cs.bufHW;
    audio.queueFrames = std::max(512, g_proj.cs.bufSW * 2);
    int selected = g_proj.cs.outDev;
    if (selected == -1 && plat::isWindows) selected = pickDefaultDevice(g_outDevs, "WASAPI", false);
    if (selected >= 0) {
        audio.deviceLabel = "<selected device missing>";
        for (const auto& device : g_outDevs) if (device.id == selected) { audio.deviceLabel = device.name; break; }
    }
    attachScRealtimeSource(audio);
    return audio;
}
#include "app.h"
bool scHasSharedAudioOwner() {
    return (g_playing && g_useCsoundApi) || g_pdEngineOn || g_pdPreview.active();
}
#include "app.h"
void closeScSoloOutput() {
    if (g_scSoloOutput) { g_scSoloOutput->close(); g_scSoloOutput.reset(); }
    g_scSoloCapture.reset();
    g_scSoloSnapshotSeq = 0;
    // Every engine that takes the soundcard calls this first: the VST engine's own stream closes
    // too (the engine itself keeps running and is mixed into the stream that opens next).
    if (g_vstSoloOutput) { g_vstSoloOutput->close(); g_vstSoloOutput.reset(); }
    g_vstSoloCapture.reset();
    g_vstSoloSnapshotSeq = 0;
    closeSampleOutput();   // the sample engine's own stream lets go as well; it re-opens when the card is free again
}
#include "app.h"
void handAudioCardToNextStream() {
    if (g_vstSoloOutput) { g_vstSoloOutput->close(); g_vstSoloOutput.reset(); }
    g_vstSoloCapture.reset();
    g_vstSoloSnapshotSeq = 0;
    if (g_scSoloOutput) { g_scSoloOutput->close(); g_scSoloOutput.reset(); }
    g_scSoloCapture.reset();
    g_scSoloSnapshotSeq = 0;
    closeSampleOutput();
}
#include "app.h"
bool startScSoloOutput(std::string& error) {
    if (!g_scEngine.embedded() || scHasSharedAudioOwner()) return true;
    if (g_scSoloOutput) return true;
    // One stream at a time: the sample engine's own stream must let go of the device first -
    // its mix continues in the SC stream through the shared realtime hook. Two open streams
    // would pump the shared hook twice per device period: everything plays at double speed
    // with the two copies interleaving as noise.
    if (g_sampleOutput) { g_sampleOutput->close(); g_sampleOutput.reset(); g_sampleCapture.reset(); logLine("[caw] Sample stream closed: the SuperCollider stream now carries the sample mix."); }
    // The VST engine's own stream (Run VST alone) must let go of the device first: its mix
    // continues in the SC stream through the shared realtime hook.
    if (g_vstSoloOutput) { g_vstSoloOutput->close(); g_vstSoloOutput.reset(); }
    g_vstSoloCapture.reset();
    g_vstSoloSnapshotSeq = 0;
    if (!g_audioDevicesReady) refreshAudioDevices(false);
    if (g_proj.cs.outDev == -2) { error = "Select an audio output in Settings > Audio."; return false; }
    auto output = std::make_unique<CawAudioOutput>();
    CawAudioSettings settings = pdAudioSettings();
    auto capture = std::make_unique<MonitorCapture>(settings.sampleRate, settings.channels);
    output->setMonitorCapture(capture.get());
    if (!output->open(settings, error)) return false;
    output->setExpectingAudio(false);
    g_scSoloCapture = std::move(capture);
    g_scSoloOutput = std::move(output);
    if (g_scSoloOutput->actualRate() != settings.sampleRate)
        logLine("[caw] WARNING: the output device runs at " + std::to_string(g_scSoloOutput->actualRate()) + " Hz but the project is " + std::to_string(settings.sampleRate) + " Hz - sounds play at the wrong speed/pitch. Set the project sample rate to the device rate in Settings > Audio.");
    return true;
}
#include "app.h"
void restoreScSoloOutput() {
    if (g_scEngine.embedded() && !scHasSharedAudioOwner() && !g_scSoloOutput) {
        std::string error;
        if (!startScSoloOutput(error)) logLine("[caw] SuperCollider output could not restart: " + error);
    }
    ensureVstSoloOutput();   // no-op unless the VST engine runs and no other stream carries audio
    ensureSampleOutput();    // the same for the sample engine (Run sample on, card free again)
}
#include "app.h"
std::filesystem::path pdConfigPath() {
    std::string home = plat::getEnv(plat::isWindows ? "USERPROFILE" : "HOME");
    return home.empty() ? std::filesystem::path() : std::filesystem::u8path(home) / ".caw_libpd_path";
}
#include "app.h"
std::string scBootstrap(int outputChannels, int serverPort) {
    const int physicalChannels = std::max(1, outputChannels);
    const int serverOutputBuses = physicalChannels + ScEngine::slotStride * ScEngine::maxTrackSlots;
    std::ostringstream out;
    out << "(\n"
        << "o = ServerOptions.new; o.maxLogins = 32; o.numOutputBusChannels = " << serverOutputBuses
        << "; o.numInputBusChannels = " << ScEngine::kInputChannels << "; o.numAudioBusChannels = " << std::max(1024, serverOutputBuses + 256) << ";\n"
        << "~cawserveraddr = NetAddr(\"127.0.0.1\", " << serverPort << ");\n"
        // CAW owns the embedded server's life; sclang's own alive-check must not decide whether it "is running" (it flips
        // to false whenever the audio callback has not pumped the server for a moment, and then Function.play refuses
        // with "Server 'caw' not running"). So: stop the alive thread and keep serverRunning true.
        << "~cawforcerunning = { try { s.stopAliveThread } { |e| e.reportError }; try { s.statusWatcher.serverRunning_(true) } { |e| e.reportError }; };\n"
        << "~cawreadyosc = OSCFunc({ |msg| ~cawreadyosc.free; ~cawforcerunning.value; \"CAW_SC_READY\".postln; }, '/status.reply', ~cawserveraddr);\n"
        << "s = Server.remote(\\caw, ~cawserveraddr, o);\n"
        << "Server.default = s;\n"
        << "s.notify; s.startAliveThread(0.1);\n"
        << "s.sendStatusMsg;\n"
        << "~cawgroups = IdentityDictionary.new; ~cawcancelled = IdentityDictionary.new; ~cawgeneration = 0;\n"
        << "~cawtrackbuses = IdentityDictionary.new;\n"
        << "~cawnullbus = 255;   // the play wrapper's own Out: nothing meters or mixes this bus, so code with explicit Out.ar does not double\n"
        << "OSCdef(\\cawStart, { |msg|\n"
        << "    var id = msg[1].asInteger, path = msg[2].asString, trackSlot = msg[7].asInteger, clipId = msg[8].asInteger;\n"
        << "    var delay = msg[3].asFloat.max(0), duration = msg[4].asFloat.max(0.001);\n"
        << "    var p4 = msg[5].asFloat, p5 = msg[6].asFloat, outBus = 0, generation;\n"
        << "    generation = ~cawgeneration;\n"
        << "    if ((trackSlot >= 0) and: { trackSlot < " << ScEngine::kFirstBusSlot << " }) {\n"
        << "        outBus = ~cawtrackbuses[trackSlot] ?? { var index = " << physicalChannels << " + (trackSlot * " << ScEngine::slotStride << "); ~cawtrackbuses[trackSlot] = index; index; };\n"
        << "    };\n"
        << "    ~cawcancelled[id] = false;\n"
        << "    SystemClock.sched(delay, {\n"
        << "        var group;\n"
        << "        if ((generation == ~cawgeneration) and: { ~cawcancelled[id] != true }) {\n"
        << "            group = Group.tail(s); ~cawgroups[id] = group; ~cawgroup = group; ~cawoutbus = outBus;\n"
        << "            ~cawtrack = trackSlot;   // the lane namespace of THIS track: ~cawget reads t<slot>_<name>, so two tracks never share a lane\n"
        << "            ~cawp4 = p4; ~cawp5 = p5; ~cawnoteduration = duration;\n"
        << "            if (s.serverRunning.not) { ~cawforcerunning.value; \"CAW_SC_FORCED_RUNNING\".postln; };\n"
        << "            (\"CAW_SC_CLIP_BEGIN \" ++ id.asString ++ \" \" ++ clipId.asString).postln;\n"
        << "            try { thisProcess.interpreter.executeFile(path); } { |error| (\"CAW_SC_CLIP_ERROR \" ++ id.asString ++ \" \" ++ error.asString).postln; error.reportError; };\n"
        << "            (\"CAW_SC_CLIP_END \" ++ id.asString).postln;\n"
        << "            SystemClock.sched(duration, { group.free; ~cawgroups.removeAt(id); ~cawcancelled.removeAt(id); nil; });\n"
        << "        };\n"
        << "        nil;\n"
        << "    });\n"
        << "}, '/caw/start');\n"
        << "OSCdef(\\cawStop, { |msg|\n"
        << "    var id = msg[1].asInteger, group = ~cawgroups[msg[1].asInteger];\n"
        << "    ~cawcancelled[id] = true; if (group.notNil) { group.free; ~cawgroups.removeAt(id); };\n"
        << "}, '/caw/stop');\n"
        << "OSCdef(\\cawStopAll, { |msg| ~cawgeneration = ~cawgeneration + 1; ~cawgroups.values.do { |group| group.free; }; ~cawgroups.clear; ~cawcancelled.clear; }, '/caw/stopAll');\n"
        << "OSCdef(\\cawResume, { |msg| s.sendStatusMsg; ~cawforcerunning.value; \"CAW_SC_READY\".postln; }, '/caw/resume');\n"
        << "~cawr = IdentityDictionary.new;   // CAW hub (caws/cawr): SC clips read ~cawr[\\name]\n"
        // CAW AUDIO BUSES (caw_audio_bus.h): ~cawbsend.("name", sig) writes a stereo signal into the named
        // bus (CAW moves it to Csound / PD / the other engines); ~cawbrecv.("name") reads one (2 channels).
        // CAW maps every name the SC code uses to a reserved send slot / input pair (/caw/busmap).
        << "~cawbussendbase = " << physicalChannels + ScEngine::kFirstBusSlot * ScEngine::slotStride << "; ~cawbusslotstride = " << ScEngine::slotStride << ";\n"
        << "~cawbsendidx = IdentityDictionary.new; ~cawbrecvidx = IdentityDictionary.new;\n"
        << "OSCdef(\\cawBusMap, { |msg| var name = msg[1].asSymbol, kind = msg[2].asInteger, index = msg[3].asInteger;\n"
        << "    if (kind < 0) { ~cawbsendidx.clear; ~cawbrecvidx.clear; } { if (kind == 0) { ~cawbsendidx[name] = index } { ~cawbrecvidx[name] = index } };\n"
        << "}, '/caw/busmap');\n"
        << "~cawbsend = { |name, sig| var k = ~cawbsendidx[name.asSymbol]; if (k.isNil) { (\"CAW: no audio bus slot for \" ++ name).postln; 0 } { Out.ar(~cawbussendbase + (k * ~cawbusslotstride), if (sig.size == 0) { [sig, sig] } { sig }) } };\n"
        << "~cawbrecv = { |name| var k = ~cawbrecvidx[name.asSymbol]; if (k.isNil) { (\"CAW: no audio bus input for \" ++ name).postln; Silent.ar(2) } { In.ar(NumOutputBuses.ir + " << ScEngine::kMicChannels << " + (k * 2), 2) } };\n"
        << "~cawbuses = IdentityDictionary.new;   // one control bus per automation channel, CAW keeps it current like the lane dots\n"
        << "~cawkey = { |name| if (~cawtrack.isNil) { name.asSymbol } { (\"t\" ++ ~cawtrack.asString ++ \"_\" ++ name.asString).asSymbol } };   // the lane key: per-track (t<slot>_<name>) while a clip dispatch is in scope, bare name otherwise\n"
        << "~cawbus = { |name| var key = ~cawkey.(name), bus = ~cawbuses[key]; if (bus.isNil) { bus = Bus.control(s, 1); bus.value = ~cawr[key] ? 0; ~cawbuses[key] = bus; }; bus; };   // cawbus: a live control bus for a lane, read it with In.kr(bus.index, 1)\n"
        << "OSCdef(\\cawHub, { |msg| var key = msg[1].asSymbol, value = msg[2], bus = ~cawbuses[key]; ~cawr[key] = value; if (bus.notNil) { bus.value = value; }; }, '/caw/hub');\n"
        << "~cawget = { |name| if (UGen.buildSynthDef.notNil) { In.kr(~cawbus.(name).index, 1) } { ~cawr[~cawkey.(name)] ? 0 } };   // cawget: inside a synth it is a live k-rate control signal on the lane bus (like Csound cawget); outside it reads the lane value once\n"
        << "OSCdef(\\cawQuit, { |msg|\n"
        << "    ~cawgeneration = ~cawgeneration + 1; ~cawgroups.values.do { |group| group.free; }; ~cawgroups.clear; ~cawcancelled.clear;\n"
        << "    ~cawtrackbuses.clear;\n"
        << "    SystemClock.sched(0.05, { 0.exit; nil; });\n"
        << "}, '/caw/quit');\n"

        << ")\n";
    return out.str();
}
#include "app.h"
void shareEngineDlls() {
    const std::string csoundExe = csoundExePath();
    std::vector<std::string> dirs;
    auto addFolderOf = [&](const std::string& file) {
        if (!file.empty()) dirs.push_back(std::filesystem::u8path(file).parent_path().u8string());
    };
    addFolderOf(csoundExe);
    addFolderOf(scSynthLibraryPath());
    addFolderOf(g_pdLibraryPath.empty() ? plat::bundledLibpd() : g_pdLibraryPath);
    std::string key;
    for (const std::string& dir : dirs) key += dir + "|";
    static std::string lastKey;
    if (key == lastKey) return;
    lastKey = key;
    for (const std::string& line : plat::preloadNewestSharedDlls(dirs)) logLine(line);
    // A Csound copy inside CAW's own folder is not on PATH. Csound loads its opcode plugins with a plain
    // LoadLibrary, so plugin dependencies can only come from DLLs already in this process (or on PATH).
    // libpd, loaded later for Csound + PD, ships its own copies of several runtime DLL names, and Windows
    // gives every name only to the first engine that asks - that is how Csound + PD could fail with the
    // copied Csound while each engine alone worked fine. Load every DLL of the Csound folder and of its
    // plugin folder by full path, so Csound gets its own copies exactly like in a Csound-only run.
    for (const std::string& line : plat::preloadAllDllsOf(std::filesystem::u8path(csoundExe).parent_path().u8string())) logLine(line);
    for (const std::string& line : plat::preloadAllDllsOf(plat::csoundPluginsDir(csoundExe))) logLine(line);
    // Always visible, so it is immediately obvious WHICH build is running and WHICH Csound was found
    // (a stale exe once made the same error reappear for days).
    logLine("[caw] Engine DLL setup done - Csound: " + (csoundExe.empty() ? std::string("(none found)") : csoundExe));
}
#include "app.h"
bool startScEngine() {
    auto failed = [](const std::string& text) {
        logLine("[caw] " + text);
        playNotice(text, "SuperCollider failed: see log");
        return false;
    };
    if (!g_langSc) return failed("Enable SuperCollider in Settings > Engines first.");
    if (g_scEngineOn) return true;
    if ((g_playing && !g_useCsoundApi) || g_testSound.running() || g_renderActive) {
        return failed("Stop the legacy Csound process, test sound, or export before starting embedded SuperCollider.");
    }
    if (!g_audioDevicesReady) refreshAudioDevices(false);
    const std::string library = scSynthLibraryPath();
    if (library.empty()) {
        return failed("libscsynth was not found. Choose it under Settings > Programs, or build the CAW SuperCollider runtime.");
    }
    shareEngineDlls();
    std::string error;
    g_scEngine.setGain(1.f);   // the master chain applies the master gain on the device buffer (no double gain)
    const int scOutputChannels = ScEngine::serverOutputChannels;
    const bool resumeLanguage = g_scEngine.canReuseWorld(library, g_proj.cs.sr, scOutputChannels) &&
                                g_scEngine.running();
    if (g_scEngine.active() && !resumeLanguage) g_scEngine.stopLanguage();
    if (!g_scEngine.startEmbedded(library, g_proj.cs.sr, scOutputChannels, error)) {
        return failed("Embedded SuperCollider server failed: " + error);
    }
    if (!resumeLanguage) {
        std::error_code ec;
        const auto bootstrap = workDir() / "caw_supercollider_boot.scd";
        std::filesystem::create_directories(bootstrap.parent_path(), ec);
        if (ec || !writeTextFile(bootstrap.string(), scBootstrap(scOutputChannels, g_scEngine.boundServerPort()))) {
            g_scEngine.setRendering(false);
            return failed("Could not write the SuperCollider startup script.");
        }
            const std::string scConf = writePrivateSclangConf();
            if (!g_scEngine.start(scLangExePath(), bootstrap.string(), error, scConf)) {
            g_scEngine.setRendering(false);
            return failed("sclang did not start: " + error);
        }
    }
    g_scEngineOn = true;
    g_scReady = false;
    g_scActiveInstances.clear();
    g_scTimelineSig = 0;
    g_scPlanT = 0.0;
    g_scLastPDTimelineTime = -1.0;
    g_scStartTime = std::chrono::steady_clock::now();
    if (!scHasSharedAudioOwner() && !startScSoloOutput(error)) {
        g_scEngine.setRendering(false);
        g_scEngineOn = false;
        return failed("SuperCollider could not open CAW's audio output: " + error);
    }
    if (resumeLanguage && !g_scEngine.resumeLanguage()) {
        closeScSoloOutput();
        g_scEngine.setRendering(false);
        g_scEngineOn = false;
        return failed("Could not resume the existing sclang connection.");
    }
    logLine("[caw] Embedded SuperCollider is running inside CAW's shared audio callback; connecting sclang...");
    return true;
}
#include "app.h"
void stopScEngine() {
    if (!g_scEngineOn) return;
    closeScSoloOutput();
    (void)g_scEngine.stopAll();
    g_scEngine.setRendering(false);
    g_scEngineOn = false; g_scReady = false; g_scPendingClipId = 0;
    g_scActiveInstances.clear();
    g_scTimelineSig = 0;
    if (g_playNotice.find("SuperCollider timed out") != std::string::npos) {
        g_playNotice.clear();
        g_playNoticeT = 0.0;
    }
    if (g_timelineRun && !engineIsUp() && !g_pdTimelineMode && !vstSolo().audible()) g_timelineRun = false;
    ensureVstSoloOutput();   // the VST engine gets its own stream back; PD / Csound keep theirs
    logLine("[caw] SuperCollider stopped.");
}
#include "app.h"
int newScInstanceId() {
    if (g_scNextInstanceId < 1 || g_scNextInstanceId >= 2000000000) g_scNextInstanceId = 1;
    while (g_scActiveInstances.count(g_scNextInstanceId)) ++g_scNextInstanceId;
    return g_scNextInstanceId++;
}
#include "app.h"
bool sendScClip(const Clip& clip, double delay, double duration, double p4, double p5, int trackSlot, const std::string& codeOverride )
{
    if (!g_scReady || !g_scEngine.running()) return false;
    if (clip.code.empty() && codeOverride.empty()) { logLine("[caw] SuperCollider clip " + clip.name + " has no code."); return false; }
    // The timeline launches from the code SNAPSHOT taken when the transport started (and updated
    // only by Run): code the user is typing right now must not reach a (re)launch on its own.
    std::string routedCode = codeOverride.empty() ? clip.code : codeOverride;
    static const std::regex playTarget(R"re(\.play\s*\(\s*target\s*:\s*~caw[Gg]roup)re");
    static const std::regex numericOut(R"re(\bOut\.ar\(\s*(\d+)\s*,)re");
    static const std::regex hasOut(R"re(\bOut\.ar\s*\(|~cawbsend\s*\.)re");   // ~cawbsend writes its own Out.ar (an audio bus send counts as an explicit out)
    // Out.ar(N, ...) addresses the TRACK's channel N (0-based): the virtual soundcard. Route every
    // numeric out to the track slot's bus (bus + N), whatever N is - Out.ar(3) is the 4th channel.
    routedCode = std::regex_replace(routedCode, numericOut, "Out.ar(~cawoutbus + $1,");
    // The Function-play wrapper ALSO writes the function's return value (Out.ar returns its input).
    // Code with its own Out.ar must not double: the wrapper goes to the null bus (never metered or
    // mixed), the code's own outs decide the channels. Without an Out.ar the wrapper is the out.
    const bool explicitOut = std::regex_search(routedCode, hasOut);
    routedCode = std::regex_replace(routedCode, playTarget,
                                    explicitOut ? ".play(outbus: ~cawnullbus, target: ~cawgroup"
                                                : ".play(outbus: ~cawoutbus, target: ~cawgroup");
    std::error_code ec;
    const auto path = workDir() / ("caw_sc_clip_" + std::to_string(clip.id) + ".scd");
    const auto cached = g_scClipFileCache.find(clip.id);
    ec.clear();
    const bool fileMissing = !std::filesystem::is_regular_file(path, ec) || ec;
    if (fileMissing || cached == g_scClipFileCache.end() || cached->second != routedCode) {
        if (!writeTextFile(path.string(), routedCode)) {
            logLine("[caw] Could not write SuperCollider clip file: " + path.string());
            return false;
        }
        g_scClipFileCache[clip.id] = routedCode;
    }
    std::string oscPath = path.generic_u8string();
    const int instance = newScInstanceId();
    const float delaySec = static_cast<float>(std::max(0.0, delay));
    const float durationSec = static_cast<float>(std::max(0.001, duration));
    if (!g_scEngine.startClip(instance, oscPath, delaySec, durationSec,
                              static_cast<float>(p4), static_cast<float>(p5), trackSlot, clip.id)) {
        logLine("[caw] Could not send the SuperCollider clip to the language process.");
        return false;
    }
    ScActiveInstance active;
    active.clipId = clip.id;
    active.until = std::chrono::steady_clock::now() + std::chrono::milliseconds(
        static_cast<long long>((delaySec + durationSec) * 1000.0f + 500.0f));
    g_scActiveInstances[instance] = active;
    return true;
}
#include "app.h"
void stopScClip(int clipId) {
    for (auto it = g_scActiveInstances.begin(); it != g_scActiveInstances.end();) {
        if (it->second.clipId == clipId) {
            (void)g_scEngine.stopClip(it->first);
            it = g_scActiveInstances.erase(it);
        } else ++it;
    }
    g_scManualRuns.erase(clipId);   // the button reflects only manual runs
}
#include "app.h"
void stopAllScClips() {
    if (g_scEngine.active()) (void)g_scEngine.stopAll();
    g_scActiveInstances.clear();
    g_scManualRuns.clear();
}
#include "app.h"
bool scClipRunning(int clipId) {
    if (g_scPendingClipId == clipId) return true;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_scActiveInstances.begin(); it != g_scActiveInstances.end();) {
        if (it->second.until <= now) it = g_scActiveInstances.erase(it);
        else { if (it->second.clipId == clipId) return true; ++it; }
    }
    return false;
}
#include "app.h"
void launchScClip(const Clip& clip, double durOverride )
{
    if (g_scReady) {
        const double p4 = clip.midiEnabled ? 60.0 : 0.0;
        const double p5 = clip.midiEnabled ? 100.0 : 0.0;
        int laneIndex = -1;
        findClip(clip.id, &laneIndex);
        const int slot = laneIndex >= 0 ? scTrackSlot(g_proj.tracks[static_cast<size_t>(laneIndex)].id) : -1;
        if (!sendScClip(clip, 0.0, durOverride > 0.0 ? durOverride : clip.dur, p4, p5, slot)) logLine("[caw] SuperCollider clip could not be launched.");
    } else if (!g_scEngineOn) {
        g_scPendingClipId = clip.id;
        if (!startScEngine()) g_scPendingClipId = 0;
    } else {
        g_scPendingClipId = clip.id;
        playNotice("SuperCollider is still booting. The clip will start when the server is ready.", "SuperCollider is booting...");
    }
}
#include "app.h"
unsigned long long scTimelineSignature(const Project& project) {
    unsigned long long h = 1469598103934665603ULL;
    auto mix = [&h](unsigned long long value) { h ^= value; h *= 1099511628211ULL; };
    for (const Track& track : project.tracks) {
        if (track.kind != 5) continue;
        mix(static_cast<unsigned long long>(track.id)); mix(track.muted); mix(track.solo);
        for (const Clip& clip : track.clips) {
            mix(static_cast<unsigned long long>(clip.id));
            mix(static_cast<unsigned long long>(std::llround(clip.start * 1000.0)));
            mix(static_cast<unsigned long long>(std::llround(clip.dur * 1000.0)));
            mix(static_cast<unsigned long long>(std::max(1, clip.runs))); mix(clip.muted); mix(clip.midiEnabled);
            // SUPERCOLLIDER-STYLE: clip.code and pFieldRules are NOT in the signature, so editing them while
            // the timeline plays does NOT re-schedule (re-launch) SC clips. The new code is applied only on
            // Alt+Enter / Run (launchScClip). Structural changes (start / dur / mute / solo / runs) still re-schedule.
            for (const MidiClip& midi : track.midiClips) if (midi.clipId == clip.id) {
                mix(static_cast<unsigned long long>(midi.id));
                mix(static_cast<unsigned long long>(std::llround(midi.start * 1000.0)));
                mix(static_cast<unsigned long long>(std::llround(midi.dur * 1000.0)));
                mix(midi.muted);
                for (const MidiNote& note : midi.notes) {
                    mix(static_cast<unsigned long long>(std::llround(note.start * 1000.0)));
                    mix(static_cast<unsigned long long>(std::llround(note.dur * 1000.0)));
                    mix(static_cast<unsigned long long>(note.pitch));
                    mix(static_cast<unsigned long long>(note.velocity));
                }
            }
        }
    }
    return h;
}
#include "app.h"
void pollScAutomation();   // defined below pollCawHub: feed the SC lanes (~cawget) while the timeline rolls
#include "app.h"
void scTimelineWindows(double from) {
    static std::map<int, bool> wasIn;   // clipId -> the playhead was inside this window at the last check
    bool anySolo = false;
    for (const Track& t : g_proj.tracks) if (t.solo && !t.muted) anySolo = true;
    std::set<int> seen;
    for (const Track& t : g_proj.tracks) {
        if (t.kind != 5) continue;
        const bool audible = !t.muted && !(anySolo && !t.solo) && !trackFrozen(t);   // frozen: the WAV plays, the live engine skips
        for (const Clip& c : t.clips) {
            seen.insert(c.id);
            const bool clipAudible = audible && !c.muted;
            const bool running = scClipRunning(c.id);
            const bool inWindow = c.start <= from + 1e-9 && from < c.start + c.dur;
            const bool was = wasIn.count(c.id) != 0 && wasIn[c.id];
            if (!clipAudible || !inWindow) {
                if (running && (!clipAudible || !was)) stopScClip(c.id);   // muted mid-window, or the playhead left it
                wasIn[c.id] = false;
                continue;
            }
            if (!was) {   // window entry: always a fresh launch, from the code SNAPSHOT
                if (running) stopScClip(c.id);
                int laneIndex = -1;
                findClip(c.id, &laneIndex);
                const int slot = laneIndex >= 0 ? scTrackSlot(g_proj.tracks[(size_t)laneIndex].id) : -1;
                const double p4 = c.midiEnabled ? 60.0 : 0.0;
                const double p5 = c.midiEnabled ? 100.0 : 0.0;
                auto snap = g_scTimelineCode.find(c.id);
                if (snap == g_scTimelineCode.end()) snap = g_scTimelineCode.emplace(c.id, c.code).first;   // a clip added mid-play: snapshot it now
                sendScClip(c, 0.0, std::max(0.05, c.start + c.dur - from), p4, p5, slot, snap->second);
            }
            wasIn[c.id] = true;
        }
    }
    for (auto it = wasIn.begin(); it != wasIn.end();)
        if (seen.count(it->first)) ++it; else it = wasIn.erase(it);
    for (auto it = g_scTimelineCode.begin(); it != g_scTimelineCode.end();)
        if (seen.count(it->first)) ++it; else it = g_scTimelineCode.erase(it);
}
#include "app.h"
void scheduleScTimeline(double from, bool restart )
{
    // The timeline RUNS the SC clips now (like Csound / VSTi / VSTe): transport start, loop wrap
    // and structural edits (re)evaluate the clip windows from the playhead. Transport STOP still
    // calls stopAllScClips directly. The Run button keeps working exactly as before (it is the
    // manual launch; the running instance is then treated as the clip's instance).
    if (restart) {
        stopAllScClips();
        // KEEP existing snapshots (they carry the code that was applied): a transport restart or a
        // loop wrap must never re-snapshot from the editors - typed code stays out until Run
        // applies it. Only clips without a snapshot yet (new since the transport started) get one.
        for (const Track& t : g_proj.tracks) {
            if (t.kind != 5) continue;
            for (const Clip& c : t.clips) g_scTimelineCode.emplace(c.id, c.code);
        }
    }
    g_scLaneSent.clear();
    scTimelineWindows(from);
    pollScAutomation();
    g_scTimelineSig = scTimelineSignature(g_proj);
    g_scPlanT = 0.0;
}
#include "app.h"
void pollScEngine(float /*dt*/) {
    if (!g_scEngine.active()) return;
    for (const std::string& line : g_scEngine.takeOutput()) {
        // CAWS <name> <value> = a SuperCollider clip sends into CAW's shared hub (caws/cawr).
        if (line.rfind("CAWS ", 0) == 0) {
            std::istringstream parsed(line.substr(5));
            std::string name;
            double value = 0;
            if (parsed >> name >> value) g_cawHub.send(name, value);
            continue;
        }
        if (line.rfind("CAW_SC_CLIP_BEGIN ", 0) == 0) {
            std::istringstream parsed(line.substr(18));
            parsed >> g_scOutputInstanceId;
            parsed >> g_scOutputClipId;
            if (g_scOutputClipId <= 0) {
                const auto active = g_scActiveInstances.find(g_scOutputInstanceId);
                if (active != g_scActiveInstances.end()) g_scOutputClipId = active->second.clipId;
            }
            g_scErrorClipId = -1;
            g_scErrorContextLines = 0;
            g_scErrorLogIndex = -1;
            g_scErrorHeadlineSeen = false;
            g_scErrorMessage.clear();
            continue;
        }
        if (line.rfind("CAW_SC_CLIP_END ", 0) == 0) {
            std::istringstream parsed(line.substr(16));
            int instance = 0;
            parsed >> instance;
            if (instance == g_scOutputInstanceId) {
                g_scOutputInstanceId = 0;
                g_scOutputClipId = 0;
                g_scErrorClipId = -1;
                g_scErrorContextLines = 0;
                g_scErrorLogIndex = -1;
                g_scErrorHeadlineSeen = false;
                g_scErrorMessage.clear();
            }
            continue;
        }
        if (line.rfind("CAW_SC_CLIP_ERROR ", 0) == 0) {
            std::istringstream parsed(line.substr(18));
            int instance = 0;
            parsed >> instance;
            std::string message;
            std::getline(parsed, message);
            while (!message.empty() && std::isspace(static_cast<unsigned char>(message.front())))
                message.erase(message.begin());
            const auto active = g_scActiveInstances.find(instance);
            const int clipId = active != g_scActiveInstances.end()
                ? active->second.clipId
                : (instance == g_scOutputInstanceId ? g_scOutputClipId : 0);
            startScError(clipId, message);
            continue;
        }
        if (!line.empty()) {
            const int contextClip = g_scErrorContextLines > 0 ? g_scErrorClipId : 0;
            const int clipId = contextClip > 0 ? contextClip : g_scOutputClipId;
            const int reportedLine = sc_diagnostics::reportedLine(line);
            std::string low = line;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            const bool errorHeadline = low.find("error:") != std::string::npos ||
                                       low.find("exception:") != std::string::npos;
            const bool warningHeadline = low.find("warning:") != std::string::npos;
            if (errorHeadline && clipId > 0) {
                if (contextClip <= 0 || g_scErrorLogIndex < 0) startScError(clipId, line);
                else if (!g_scErrorHeadlineSeen) updateScErrorHeadline(line);
                g_scErrorHeadlineSeen = true;
                g_scErrorContextLines = 16;
                if (reportedLine > 0) {
                    setScErrorLocation(reportedLine, false, line);
                } else if (const Clip* clip = findClip(clipId)) {
                    const auto likely = sc_diagnostics::inferRuntimeLine(clip->code, line);
                    if (likely.line > 0) setScErrorLocation(likely.line, likely.approximate, line);
                }
                continue;
            }
            if (contextClip > 0 && g_scErrorContextLines > 0) {
                --g_scErrorContextLines;
                if (reportedLine > 0) setScErrorLocation(reportedLine, false, line);
                LogLine trace = classifyLog("[SuperCollider] " + line);
                trace.level = kLogMessage;  // keep backtrace detail out of the incident count
                if (const Clip* clip = findClip(contextClip)) {
                    fillLoc(trace, clip->name, reportedLine, line);
                    trace.lineApprox = false;
                }
                pushLog(std::move(trace));
                continue;
            }
            if (warningHeadline && clipId > 0) {
                LogLine warning = classifyLog("[SuperCollider] " + line);
                warning.level = kLogImportant;
                if (const Clip* clip = findClip(clipId)) fillLoc(warning, clip->name, reportedLine, line);
                pushLog(std::move(warning));
                continue;
            }
        }
        if (line.find("CAW_SC_READY") != std::string::npos) {
            if (!g_scReady) {
                g_scReady = true;
                logLine("[caw] sclang connected to CAW's embedded SuperCollider server. Audio shares the CAW output stream.");
                seedScLaneDefaults();   // ~cawget hears every lane's default before the first Play
                if (g_scPendingClipId > 0) {
                    const int pending = g_scPendingClipId;
                    g_scPendingClipId = 0;
                    if (Clip* clip = findClip(pending)) {
                        launchScClip(*clip);
                        g_scManualRuns[clip->id] = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds((long long)(clip->dur * 1000.0 + 500.0));   // a Run-triggered launch that waited for the boot: manual
                    }
                }
            }
        } else if (!line.empty()) logLine("[SuperCollider] " + line);
    }
    if (g_scEngineOn && !g_scReady &&
        std::chrono::steady_clock::now() - g_scStartTime > std::chrono::seconds(10)) {
        stopScEngine();
        g_scEngine.stopLanguage();
        logLine("[caw] SuperCollider did not answer its startup status request. Check the sclang/libscsynth build and local UDP ports.");
        playNotice("SuperCollider startup timed out. Check the log and SuperCollider build.", "SuperCollider timed out: see log");
        return;
    }
    for (auto it = g_scActiveInstances.begin(); it != g_scActiveInstances.end();) {
        if (it->second.until <= std::chrono::steady_clock::now()) it = g_scActiveInstances.erase(it);
        else ++it;
    }
    if (g_scEngine.finished()) {
        const int code = g_scEngine.exitCode();
        closeScSoloOutput();
        (void)g_scEngine.stopAll();
        g_scEngine.stopLanguage();
        g_scEngine.setRendering(false);
        g_scEngineOn = false; g_scReady = false; g_scPendingClipId = 0;
        g_scActiveInstances.clear();
        if (g_timelineRun && !engineIsUp() && !g_pdTimelineMode && !vstSolo().audible()) g_timelineRun = false;
        ensureVstSoloOutput();   // the VST engine carries the session now
        logLine(code == 0 ? "[caw] SuperCollider ended." : "[caw] SuperCollider ended with exit code " + std::to_string(code) + ". See its messages above.");
    }
}
#include "app.h"
// CAW AUDIO BUSES: map the bus names the SuperCollider code uses onto the reserved send slots /
// receive pairs, keep the per-track automatic bus names current ("sc_<track>", "smp_<track>").
// Cheap when nothing changed (a signature compare); re-sent whenever SuperCollider (re)starts.
void updateCawBusRouting() {
    {   // the soundcard input as the cable "mic" (inputs 1-2), so any engine can receive the microphone
        // by name. A small UI-thread pump: it moves what was captured since the last frame.
        static cawin::Reader reader; static cawbus::Port port; static std::vector<float> blk;
        if (cawin::isOpen()) {
            port.rename("mic");
            const std::uint64_t h = cawin::state().head.load(std::memory_order_acquire);
            if (!reader.started) { reader.pos = h; reader.started = true; }
            size_t n = (size_t)std::min<std::uint64_t>(h > reader.pos ? h - reader.pos : 0, cawin::kCap / 2);
            while (n > 0) {
                const size_t chunk = std::min<size_t>(n, 1024);
                if (blk.size() < chunk * 2) blk.resize(chunk * 2);
                reader.read(blk.data(), chunk, 2, 0);
                cawbus::send(port, blk.data(), chunk);
                n -= chunk;
            }
        }
    }
    {   // automatic per-track buses
        std::map<int, std::string> smp, sc;
        for (const Track& t : g_proj.tracks) {
            if (t.kind == 7) smp[t.id] = "smp_" + cawbus::safeName(t.name);
            if (t.kind == 5) { const int slot = scTrackSlot(t.id); if (slot >= 0) sc[slot] = "sc_" + cawbus::safeName(t.name); }
        }
        { std::lock_guard<std::mutex> l(cawbus::sampleTrackNames().m); if (cawbus::sampleTrackNames().byKey != smp) cawbus::sampleTrackNames().byKey = smp; }
        { std::lock_guard<std::mutex> l(cawbus::scSlotNames().m); if (cawbus::scSlotNames().byKey != sc) cawbus::scSlotNames().byKey = sc; }
    }
    static const std::regex sendRe(R"re(~cawbsend\s*\.\s*\(\s*"([A-Za-z0-9_-]+)")re");
    static const std::regex recvRe(R"re(~cawbrecv\s*\.\s*\(\s*"([A-Za-z0-9_-]+)")re");
    std::vector<std::string> sends, recvs;
    for (const Track& t : g_proj.tracks) if (t.kind == 5) for (const Clip& c : t.clips) {
        for (std::sregex_iterator it(c.code.begin(), c.code.end(), sendRe), end; it != end; ++it)
            if (std::find(sends.begin(), sends.end(), (*it)[1].str()) == sends.end()) sends.push_back((*it)[1].str());
        for (std::sregex_iterator it(c.code.begin(), c.code.end(), recvRe), end; it != end; ++it)
            if (std::find(recvs.begin(), recvs.end(), (*it)[1].str()) == recvs.end()) recvs.push_back((*it)[1].str());
    }
    std::array<std::string, ScEngine::kBusSlots> sa{};
    std::array<std::string, ScEngine::kRecvPairs> ra{};
    for (size_t k = 0; k < sends.size() && k < sa.size(); ++k) sa[k] = sends[k];
    for (size_t k = 0; k < recvs.size() && k < ra.size(); ++k) ra[k] = recvs[k];
    static std::string lastSig; static bool lastUp = false; static bool warned = false;
    std::string sig;
    for (const auto& n : sa) sig += n + "|";
    sig += "#";
    for (const auto& n : ra) sig += n + "|";
    const bool up = g_scReady && g_scEngine.running();
    if (sig == lastSig && up == lastUp) return;
    lastSig = sig; lastUp = up;
    g_scEngine.setBusRouting(sa, ra);
    if (!up) return;
    g_scEngine.sendBusMap(std::string(), -1, 0);
    for (int k = 0; k < (int)sa.size(); ++k) if (!sa[(size_t)k].empty()) g_scEngine.sendBusMap(sa[(size_t)k], 0, k);
    for (int k = 0; k < (int)ra.size(); ++k) if (!ra[(size_t)k].empty()) g_scEngine.sendBusMap(ra[(size_t)k], 1, k);
    if (!warned && (sends.size() > sa.size() || recvs.size() > ra.size())) {
        warned = true;
        logLine("[caw] SuperCollider audio buses: at most " + std::to_string(sa.size()) + " sends and " + std::to_string(ra.size()) + " receives - the extra names stay silent.");
    }
}
#include "app.h"
// THE SOUNDCARD INPUT: opened once (the project's input device, nchnls_i or 2 channels) whenever an
// engine runs and the project asks for input (an input device is chosen, or any clip uses inch /
// ins / adc~ / SoundIn / the "mic" cable). Also streams inputs 1-2 into the CAW cable "mic".
void ensureCawAudioInput() {
    bool wanted = g_proj.cs.inDev >= 0 || g_proj.cs.nchnlsIn > 0;
    if (!wanted)
        for (const Track& t : g_proj.tracks) for (const Clip& c : t.clips) {
            const std::string& k = c.code;
            if (k.find("inch") != std::string::npos || k.find(" ins") != std::string::npos || k.find("SoundIn") != std::string::npos ||
                k.find("\"mic\"") != std::string::npos || k.find("cawr~ mic") != std::string::npos) { wanted = true; break; }
            if (t.kind == 6 && k.size() > 3 && k.substr(k.size() - 3) == ".pd") {
                std::ifstream in(std::filesystem::u8path(k.find(':') == std::string::npos && !g_projectPath.empty()
                    ? (std::filesystem::u8path(g_projectPath).parent_path() / std::filesystem::u8path(k)).u8string() : k), std::ios::binary);
                std::string txt((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (txt.find("adc~") != std::string::npos) { wanted = true; break; }
            }
        }
    if (!wanted || cawin::isOpen()) return;
    std::string err;
    const int ch = g_proj.cs.nchnlsIn > 0 ? g_proj.cs.nchnlsIn : 2;
    if (!cawin::open(g_proj.cs.inDev, g_proj.cs.sr, ch, err)) { logLine("[caw] Audio input: " + err); return; }
    logLine("[caw] Audio input open: \"" + cawin::state().device + "\", " + std::to_string(cawin::channels()) + " ch - adc~ / inch / SoundIn, and the cable \"mic\".");
}
#include "app.h"
void pollCawHub() {
    updateCawBusRouting();
    static std::uint64_t seen = 0;
    std::map<std::string,double> values;
    if (!g_cawHub.take(seen, values)) return;
    if (!g_scEngine.running()) return;
    for (const auto& entry : values)
        g_scEngine.sendHub(entry.first, static_cast<float>(entry.second));
}
#include "app.h"
// Seed every SuperCollider automation lane's DEFAULT value into the language right after the
// engine boots (user report: ~cawget read 0 for a lane without dots until Play started). After
// this, ~cawget returns the lane's default even while the transport is stopped.
void seedScLaneDefaults() {
    for (const Track& t : g_proj.tracks) {
        if (t.kind != 5) continue;
        const int slot = scTrackSlot(t.id);
        if (slot < 0) continue;
        for (const auto& lane : t.automation) {
            const std::string& chn = lane.first;
            if (!autoLaneActive(t, chn)) continue;
            double lo = 0.0, hi = 1.0, initial = 0.5;
            const auto range = t.automationRanges.find(chn);
            if (range != t.automationRanges.end()) {
                lo = range->second.min;
                hi = range->second.max > lo ? range->second.max : lo + 1.0;
                initial = range->second.value;
            }
            const float value = static_cast<float>(autoValueAt(lane.second, g_play, initial));
            const std::string hubName = "t" + std::to_string(slot) + "_" + chn;
            g_scLaneSent[hubName] = value;
            g_scEngine.sendHub(hubName, value);
        }
    }
}
#include "app.h"
void pollScAutomation() {
    if (!g_scReady || !g_scEngine.running() || !g_timelineRun) return;
    for (const Track& t : g_proj.tracks) {
        if (t.kind != 5) continue;
        if (trackFrozen(t)) continue;   // frozen: the automation values are baked into the WAV, nothing streams
        for (const auto& lane : t.automation) {
            const std::string& chn = lane.first;
            const std::vector<AutoPoint>& points = lane.second;
            // A lane WITHOUT dots streams its DEFAULT value (user report: ~cawget used to read 0
            // until dots existed): autoValueAt returns the fallback for an empty lane, so the clip
            // hears the lane's default from the very first block.
            if (!autoLaneActive(t, chn)) continue;
            double lo = 0.0, hi = 1.0, initial = 0.5;
            const auto range = t.automationRanges.find(chn);
            if (range != t.automationRanges.end()) {
                lo = range->second.min;
                hi = range->second.max > lo ? range->second.max : lo + 1.0;
                initial = range->second.value;
            }
            const float value = static_cast<float>(autoValueAt(points, g_play, initial));
            // Per-track namespace: the language reads a lane as t<slot>_<name> inside a clip's
            // synths (~cawtrack is set by the /caw/start dispatch), so two tracks that both have
            // an "input1" lane stream separately instead of sharing one bus.
            const int slot = scTrackSlot(t.id);
            const std::string hubName = "t" + std::to_string(slot) + "_" + chn;
            const bool first = g_scLaneSent.find(hubName) == g_scLaneSent.end();
            float& seen = g_scLaneSent[hubName];
            if (seen == value) continue;
            seen = value;
            if (first) logLine("[caw] SC automation: track \"" + t.name + "\" lane \"" + chn + "\" streams into the language (t" + std::to_string(slot) + "_" + chn + ", ~cawget is live)");
            g_scEngine.sendHub(hubName, value);
        }
    }
}
#include "app.h"
// Every audio bus a Csound clip names: cawbsend "x" / cawbrecv "x" (and the project-wide code). The
// Csound engine resolves cawbo_<x>_L/R (send) and cawbi_<x>_L/R (receive) channels for exactly these.
void collectCawBusNames(std::vector<std::string>& sends, std::vector<std::string>& recvs) {
    static const std::regex sendRe(R"re(\bcawbsend[ \t]+"([A-Za-z0-9_-]+)")re");
    static const std::regex recvRe(R"re(\bcawbrecv[ \t]+"([A-Za-z0-9_-]+)")re");
    std::set<std::string> s, r;
    auto scan = [&](const std::string& code) {
        for (std::sregex_iterator it(code.begin(), code.end(), sendRe), end; it != end; ++it) s.insert((*it)[1].str());
        for (std::sregex_iterator it(code.begin(), code.end(), recvRe), end; it != end; ++it) r.insert((*it)[1].str());
    };
    for (const Track& t : g_proj.tracks) if (t.kind == 0) for (const Clip& c : t.clips) scan(c.code);
    scan(g_proj.orchestraCode); scan(g_proj.udoCode);
    for (const std::string& src : g_proj.instrPool) scan(src);
    sends.assign(s.begin(), s.end()); recvs.assign(r.begin(), r.end());
}
#include "app.h"
std::vector<std::string> collectCawHubNames() {
    static const std::regex sendRe(R"re((?:^|\n)[ \t]*caws[ \t]+[^,;\r\n]+,[ \t]*"([A-Za-z0-9_/-]+)")re");
    static const std::regex recvRe(R"re((?:^|\n)[ \t]*[ki][A-Za-z0-9_]+[ \t]+cawr[ \t]+"([A-Za-z0-9_/-]+)")re");
    std::set<std::string> names;
    for (const Track& t : g_proj.tracks)
        for (const Clip& c : t.clips) {
            for (std::sregex_iterator it(c.code.begin(), c.code.end(), sendRe), end; it != end; ++it)
                names.insert((*it)[1].str());
            for (std::sregex_iterator it(c.code.begin(), c.code.end(), recvRe), end; it != end; ++it)
                names.insert((*it)[1].str());
        }
    return {names.begin(), names.end()};
}
#include "app.h"
void runJoinTool();
#include "app.h"
std::string resolveProgram(const std::string& typed, const char* bareName, const char* folderPrefix, const std::vector<std::string>& relPaths, const char* exeName) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!typed.empty() && typed != bareName && looksLikeFilePath(typed) && fs::exists(typed, ec)) return typed;
    const std::string found = findInstalledExe(folderPrefix, relPaths, exeName);
    if (!found.empty()) return found;
    return typed.empty() ? std::string(bareName) : typed;   // not found anywhere: the start error tells the user what to do
}
#include "app.h"
bool ensureCsoundMathProbe(bool force )
{
    const std::string exe = csoundExePath();
    if (g_mathProbeDone && !force && g_mathProbeExe == exe) return g_mathHasAbs || g_mathHasSqrt;
    g_mathProbeDone = true; g_mathProbeExe = exe; g_mathProbeDetail.clear();
    std::string error;
    const bool probed = g_apiProc.probeMath(exe, g_mathHasAbs, g_mathHasSqrt, g_mathHasPow, error);
    if (!probed) { g_mathHasAbs = g_mathHasSqrt = g_mathHasPow = false; g_mathProbeDetail = error; }
    else if (!g_mathHasAbs || !g_mathHasSqrt || !g_mathHasPow) g_mathProbeDetail = error;
    setCsoundMathOps(g_mathHasAbs, g_mathHasSqrt, g_mathHasPow);
    logLine("[caw] Csound math check (" + (exe.empty() ? std::string("(none found)") : exe) + "): "
            + (g_mathHasAbs ? "abs ok" : "abs MISSING")
            + (g_mathHasSqrt ? ", sqrt ok" : ", sqrt MISSING")
            + (g_mathHasPow ? ", pow ok" : ", pow missing"));
    if (!g_mathProbeDetail.empty()) logLine("[caw] Csound math check: " + g_mathProbeDetail);
    return g_mathHasAbs || g_mathHasSqrt;
}
#include "app.h"
double engineOutputLatency();
#include "app.h"
void closeMonitorSocket() {
    if (g_monitorSocket != INVALID_SOCKET) { closesocket(g_monitorSocket); g_monitorSocket = INVALID_SOCKET; }
#ifdef _WIN32
    if (g_monitorWsa) { WSACleanup(); g_monitorWsa = false; }
#endif
    g_cawMonitorPort = 0;
    g_haveOscRawSeq = g_haveOscMeterSeq = false;
}
#include "app.h"
bool openMonitorSocket() {
    closeMonitorSocket();
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g_monitorWsa = true;
#endif
    g_monitorSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_monitorSocket == INVALID_SOCKET) { closeMonitorSocket(); return false; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int receiveSize = 2 * 1024 * 1024;
    setsockopt(g_monitorSocket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receiveSize), sizeof(receiveSize));
#ifdef _WIN32
    u_long nonblocking = 1;
    const bool nonblockOk = ioctlsocket(g_monitorSocket, FIONBIO, &nonblocking) == 0;
#else
    const int flags = fcntl(g_monitorSocket, F_GETFL, 0);
    const bool nonblockOk = flags >= 0 && fcntl(g_monitorSocket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    if (bind(g_monitorSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || !nonblockOk) {
        closeMonitorSocket(); return false;
    }
    SockLen addrLen = sizeof(addr);
    if (getsockname(g_monitorSocket, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) {
        closeMonitorSocket(); return false;
    }
    g_cawMonitorPort = ntohs(addr.sin_port);
    return g_cawMonitorPort > 0;
}
#include "app.h"
void noteRealtimeOpcodeError(const std::string& line) {
    static const std::regex missing(R"re((?:opcode entry for|unknown opcode)\s+['\"]?([A-Za-z_]\w*)['\"]?)re");
    std::smatch m;
    if (!std::regex_search(line, m, missing)) return;
    const std::string name = m[1].str();
    if (name == "abs") g_rtMissingAbs = true;
    else if (name == "sqrt") g_rtMissingSqrt = true;
    else if (name == "pow") g_rtMissingPow = true;
}
#include "app.h"
void drainPlayOutput() {
    const auto lines = g_useCsoundApi ? g_apiProc.takeOutput() : g_playProc.takeOutput();
    for (const std::string& l : lines) {
        if (handleMonitorLine(l)) continue;
        noteRealtimeOpcodeError(l);
        logCsoundLine(l);
    }
    pollMonitor();
}
#include "app.h"
// How many meters an SC track shows: the widest channel count its clips write (user request:
// the meter follows the code). Detected from the code text: Out.ar(bus, [a, b, c]) = 3,
// the multichannel expansion '! 4' = 4, a bare mono Out.ar = 1; default 2 (stereo).
int scTrackMeterChannels(const Track& t) {
    int channels = 0;
    for (const Clip& c : t.clips) {
        int ch = 1;
        // Out.ar(<anything>, <second arg>) up to the closing paren of the call
        size_t at = c.code.find("Out.ar");
        while (at != std::string::npos) {
            const size_t open = c.code.find('(', at);
            if (open != std::string::npos) {
                int depth = 0; size_t i = open;
                for (; i < c.code.size(); ++i) {
                    if (c.code[i] == '(') ++depth;
                    else if (c.code[i] == ')') { --depth; if (depth == 0) break; }
                }
                if (i < c.code.size()) {
                    // the top-level comma inside the call splits bus from channels
                    int topCommas = 0; size_t commaAt = std::string::npos; int d2 = 0;
                    for (size_t k = open + 1; k < i; ++k) {
                        const char q = c.code[k];
                        if (q == '[' || q == '(') ++d2;
                        else if (q == ']' || q == ')') --d2;
                        else if (q == ',' && d2 == 0) { ++topCommas; if (commaAt == std::string::npos) commaAt = k; }
                    }
                    // Out.ar(N, ...) with a plain number: the code writes channel N (0-based) -
                    // Out.ar(3, x) is the 4th output, so the meter count is N + 1
                    if (commaAt != std::string::npos) {
                        const std::string first = c.code.substr(open + 1, commaAt - open - 1);
                        const size_t f0 = first.find_first_not_of(" \t");
                        const size_t f1 = first.find_last_not_of(" \t");
                        if (f0 != std::string::npos && f1 != std::string::npos) {
                            const std::string num = first.substr(f0, f1 - f0 + 1);
                            if (!num.empty() && std::all_of(num.begin(), num.end(), [](char q) { return q >= '0' && q <= '9'; }))
                                ch = std::max(ch, std::atoi(num.c_str()) + 1);
                        }
                    }
                    if (topCommas >= 1) {
                        const std::string arg = c.code.substr(commaAt + 1, i - commaAt - 1);
                        // [a, b, c] -> 3 ; anything '! N' -> N ; a bare expression -> 1
                        const size_t br0 = arg.find('[');
                        if (br0 != std::string::npos) {
                            const size_t br1 = arg.rfind(']');
                            if (br1 != std::string::npos && br1 > br0) {
                                const std::string list = arg.substr(br0 + 1, br1 - br0 - 1);
                                ch = 1; int d3 = 0;
                                for (char q : list) {
                                    if (q == '[' || q == '(') ++d3;
                                    else if (q == ']' || q == ')') --d3;
                                    else if (q == ',' && d3 == 0) ++ch;
                                }
                            }
                        } else {
                            const size_t bang = arg.find('!');
                            if (bang != std::string::npos) {
                                size_t d4 = bang + 1;
                                while (d4 < arg.size() && (arg[d4] == ' ' || arg[d4] == '	')) ++d4;
                                ch = std::max(1, std::atoi(arg.c_str() + d4));
                            }
                        }
                    }
                }
            }
            at = c.code.find("Out.ar", at + 6);
        }
        // the '.play' expansion form: { ... } ! 4 }.play (user example) - and any plain '! N'
        // multichannel expansion when the code has no Out.ar call at all
        if (ch <= 1) {
            static const std::regex playBangRe(R"re(!\s*(\d+)\s*\}\s*\)\s*\.\s*play)re");
            static const std::regex anyBangRe(R"re(!\s*(\d+))re");
            std::smatch m;
            if (std::regex_search(c.code, m, playBangRe)) ch = std::max(1, std::atoi(m[1].str().c_str()));
            else if (std::regex_search(c.code, m, anyBangRe)) ch = std::max(1, std::atoi(m[1].str().c_str()));
        }
        channels = std::max(channels, std::min(ScEngine::slotStride, ch));
    }
    return channels > 0 ? channels : 2;
}
#include "app.h"
void updateMeters(float dt) {
    for (auto& entry : g_scMeterDb)
        for (float& db : entry.second) db = std::max(-100.f, db - 36.f * dt);
    for (auto& entry : g_sampleMeterDb)
        for (float& db : entry.second) db = std::max(-100.f, db - 36.f * dt);
    if (g_scEngineOn) {
        for (const Track& track : g_proj.tracks) if (track.kind == 5) {
            const int slot = scTrackSlot(track.id);
            if (slot < 0) continue;
            auto& meter = g_scMeterDb[track.id];
            for (int channel = 0; channel < ScEngine::slotStride; ++channel) {
                const float peak = g_scEngine.takeTrackPeak(slot, channel);
                const float db = 20.f * std::log10(std::max(peak, 1e-5f));
                meter[static_cast<size_t>(channel)] = std::max(meter[static_cast<size_t>(channel)], db);
            }
        }
    }
    if (g_sampleEngineOn) {
        const auto peaks = cawaudio::engine().takeTrackPeaks();   // trackId -> output channel peaks (max since the last frame)
        for (const Track& track : g_proj.tracks) {
            if ((track.kind != 7 && !trackFrozen(track)) || trackHiddenByFolder(track)) continue;
            // how many meters this track shows: the widest output use of its clips (the saved routing
            // and the code's out1..out8 custom routing both count)
            int channels = 0;
            if (trackFrozen(track)) {
                channels = 2;   // the frozen WAV plays as one stereo clip (the meter rides its voice)
            } else for (const Clip& c : track.clips) {
                if (c.audioFile.empty()) continue;
                std::string err;
                const auto data = audioClipData(c, err);
                const auto map = audioClipOutMap(c, data);
                for (int k : map) channels = std::max(channels, k + 1);
                // the code's out<n> = in<m> routing (comment lines do not count)
                size_t pos = 0;
                while (pos < c.code.size()) {
                    size_t end = c.code.find('\n', pos);
                    if (end == std::string::npos) end = c.code.size();
                    const size_t first = c.code.find_first_not_of(" \t\r", pos);
                    if (first != std::string::npos && first < end && c.code[first] != '#') {
                        static const std::regex outRe(R"re(out([1-8])[ \t]*=)re");
                        std::smatch m;
                        std::string line = c.code.substr(pos, end - pos);
                        if (std::regex_search(line, m, outRe)) channels = std::max(channels, std::atoi(m[1].str().c_str()));
                    }
                    pos = end + 1;
                }
            }
            channels = std::max(1, std::min(8, channels));
            g_sampleMeterCh[track.id] = channels;
            auto& meter = g_sampleMeterDb[track.id];
            meter.assign((size_t)channels, -100.f);
            const auto pk = peaks.find(track.id);
            if (pk == peaks.end()) continue;
            for (int channel = 0; channel < channels; ++channel) {
                const float peak = pk->second[(size_t)channel];
                const float db = 20.f * std::log10(std::max(peak, 1e-5f));
                meter[(size_t)channel] = std::max(meter[(size_t)channel], db);
            }
        }
    }
    if (g_scSoloOutput && g_scSoloCapture) {
        MonitorCapture::Snapshot snapshot;
        if (g_scSoloCapture->take(snapshot, g_scSoloSnapshotSeq)) {
            g_scSoloSnapshotSeq = snapshot.sequence;
            g_rawFullScale = 1.f;
            g_rawSampleRate = g_scSoloOutput->actualRate() > 0 ? g_scSoloOutput->actualRate() : std::max(1, g_proj.cs.sr);   // the analyzer maps frequencies at the feed's real rate
            applyRawSamples(snapshot.left.data(), snapshot.right.data());
            if (g_peakIn.size() < snapshot.meters.size()) g_peakIn.resize(snapshot.meters.size(), 0.f);
            if (g_meterClipHold.size() < g_peakIn.size()) g_meterClipHold.resize(g_peakIn.size(), 0.f);
            for (size_t i = 0; i < snapshot.meters.size(); ++i) {
                g_peakIn[i] = snapshot.meters[i];
                if (snapshot.meters[i] >= .99426f) g_meterClipHold[i] = 1.5f;
            }
            g_monAge = 0.f;
        }
    }
    if (g_vstSoloOutput && g_vstSoloCapture) {   // Run VST alone: its own stream feeds the master meter and the scope
        MonitorCapture::Snapshot snapshot;
        if (g_vstSoloCapture->take(snapshot, g_vstSoloSnapshotSeq)) {
            g_vstSoloSnapshotSeq = snapshot.sequence;
            g_rawFullScale = 1.f;
            g_rawSampleRate = g_vstSoloOutput->actualRate() > 0 ? g_vstSoloOutput->actualRate() : std::max(1, g_proj.cs.sr);   // the analyzer maps frequencies at the feed's real rate
            applyRawSamples(snapshot.left.data(), snapshot.right.data());
            if (g_peakIn.size() < snapshot.meters.size()) g_peakIn.resize(snapshot.meters.size(), 0.f);
            if (g_meterClipHold.size() < g_peakIn.size()) g_meterClipHold.resize(g_peakIn.size(), 0.f);
            for (size_t i = 0; i < snapshot.meters.size(); ++i) {
                g_peakIn[i] = snapshot.meters[i];
                if (snapshot.meters[i] >= .99426f) g_meterClipHold[i] = 1.5f;
            }
            g_monAge = 0.f;
        }
    }
    // A master-mode ("FX only") VSTe that consumes a sample track replaces the raw sample mix:
    // silence the direct sample output while that VSTe runs (the effect's output is heard instead).
    {
        bool smpReplaced = false;
        bool anySoloT = false;
        for (const Track& t : g_proj.tracks) if (t.solo && !t.muted) anySoloT = true;
        // An effect belongs to its source's signal path (see collectVstActiveTracks): it replaces the
        // sample while THAT source is audible - soloing the source keeps its insert working.
        const auto srcAudible = [&](const Track& o) { return !o.muted && (!anySoloT || o.solo); };   // a locked source's raw mix is its WAV voice: it is suppressed like a live one
        for (const Track& vt : g_proj.tracks) {
            if (vt.kind != 4 || vt.vstOutMode != 0) continue;
            // FX only keeps consuming its sources also while the VSTe is explicitly muted: muting the
            // VSTe silences the whole chain (the raw source must not leak back to the master).
            // The VSTe muted by another track's solo is skipped: the solo rule already silences the
            // consumed source, and a soloed SOURCE must keep its raw sound.
            if (anySoloT && !vt.solo) continue;
            for (const auto& in : vt.vstInputs) {
                if (!in.on || in.mode == 2) continue;
                for (const Track& o : g_proj.tracks) {
                    if (o.kind != 7 && !trackFrozen(o)) continue;
                    const bool match = in.mode == 0 ? o.name == in.name
                        : std::any_of(o.clips.begin(), o.clips.end(), [&](const Clip& c) { return c.name == in.name; });
                    if (match && srcAudible(o)) smpReplaced = true;
                }
            }
        }
        const bool carrier = g_timelineRun && (g_vstEngineOn ? vstSolo().ready() : engineIsUp());   // the transport runs AND the VSTe's engine really renders (g_playing is Csound-only)
        g_sampleRawSuppress.store(smpReplaced && carrier, std::memory_order_relaxed);
        // Dry/wet alignment in mix mode: a latency-reporting effect (oversampling, look-ahead) delivers
        // its output later than the raw source; delay the raw dry by the same amount so they line up.
        int latComp = 0;
        if (carrier && g_vstEngineOn) {
            for (const Track& vt : g_proj.tracks) {
                if (vt.kind != 4 || vt.vstOutMode != 1 || vt.muted) continue;
                bool consumesSample = false;
                for (const auto& in : vt.vstInputs) {
                    if (!in.on || in.mode == 2) continue;
                    for (const Track& o : g_proj.tracks) {
                        if (o.kind != 7 && !trackFrozen(o)) continue;
                        const bool match = in.mode == 0 ? o.name == in.name
                            : std::any_of(o.clips.begin(), o.clips.end(), [&](const Clip& c) { return c.name == in.name; });
                        if (match && srcAudible(o)) consumesSample = true;
                    }
                }
                if (consumesSample) latComp = std::max(latComp, vstSolo().latencyFor(vt.id));
            }
        }
        g_sampleDryLatencyComp.store(std::min(latComp, 20000), std::memory_order_relaxed);   // 20000-sample cap (~0.4 s): a bogus / insane reported latency must never mute the dry
    }
    if (g_sampleOutput && g_sampleCapture) {   // Run sample alone: its own stream feeds the master meter and the scope (in the chain like the others)
        MonitorCapture::Snapshot snapshot;
        if (g_sampleCapture->take(snapshot, g_sampleSnapshotSeq)) {
            g_sampleSnapshotSeq = snapshot.sequence;
            g_rawFullScale = 1.f;
            g_rawSampleRate = g_sampleOutput->actualRate() > 0 ? g_sampleOutput->actualRate() : std::max(1, g_proj.cs.sr);   // the analyzer maps frequencies at the feed's real rate
            applyRawSamples(snapshot.left.data(), snapshot.right.data());
            if (g_peakIn.size() < snapshot.meters.size()) g_peakIn.resize(snapshot.meters.size(), 0.f);
            if (g_meterClipHold.size() < g_peakIn.size()) g_meterClipHold.resize(g_peakIn.size(), 0.f);
            for (size_t i = 0; i < snapshot.meters.size(); ++i) {
                g_peakIn[i] = snapshot.meters[i];
                if (snapshot.meters[i] >= .99426f) g_meterClipHold[i] = 1.5f;
            }
            g_monAge = 0.f;
        }
    }
    if (g_pdEngineOn) {
        MonitorCapture::Snapshot snapshot;
        if (g_pdTimelineEngine.takeSnapshot(snapshot, g_pdSnapshotSeq)) {
            g_pdSnapshotSeq = snapshot.sequence;
            // PD output is normalized and captured at the device callback.
            g_rawFullScale = 1.f;
            g_rawSampleRate = std::max(1, g_proj.cs.sr);
            applyRawSamples(snapshot.left.data(), snapshot.right.data());
            if (g_peakIn.size() < snapshot.meters.size()) g_peakIn.resize(snapshot.meters.size(), 0.f);
            if (g_meterClipHold.size() < g_peakIn.size()) g_meterClipHold.resize(g_peakIn.size(), 0.f);
            for (size_t i = 0; i < snapshot.meters.size(); ++i) {
                g_peakIn[i] = snapshot.meters[i];
                if (snapshot.meters[i] >= .99426f) g_meterClipHold[i] = 1.5f;
            }
            g_monAge = 0.f;
        }
    }
    for(auto& entry:g_pdMeterDb)
        for(float& db:entry.second)db=std::max(-100.f,db-36.f*dt);
    for(auto& entry:g_vstMeterDb)
        for(float& db:entry.second)db=std::max(-100.f,db-36.f*dt);
    if (g_vstEngineOn || g_liveCsound) {   // VST track peaks by TRACK ID, from the VST engine and from the Csound-hosted rack
        std::vector<VstRack::SoloPeak> peaks;
        if (g_vstEngineOn) vstSolo().takePeaks(peaks);
        for (const auto& p : g_apiProc.takeVstPeaks()) {
            VstRack::SoloPeak sp; sp.trackId = p.trackId;
            for (int c = 0; c < 8; ++c) sp.value[(size_t)c] = p.value[(size_t)c];
            peaks.push_back(sp);
        }
        for(const auto& track:peaks) {
            auto it=g_vstMeterDb.find(track.trackId);
            if(it==g_vstMeterDb.end())it=g_vstMeterDb.emplace(track.trackId,PdMeterArr{}).first;
            for(int ch=0;ch<(int)track.value.size();++ch) {
                const float db=20.f*std::log10(std::max(track.value[(size_t)ch],1e-5f));
                it->second[(size_t)ch]=std::max(it->second[(size_t)ch],db);
            }
        }
    }
    if(g_pdEngineOn)for(const auto& track:g_pdTimelineEngine.takePeaks()) {
        auto it=g_pdMeterDb.find(track.trackId);
        if(it==g_pdMeterDb.end())it=g_pdMeterDb.emplace(track.trackId,PdMeterArr{}).first;
        for(int ch=0;ch<(int)track.value.size();++ch) {
            const float db=20.f*std::log10(std::max(track.value[(size_t)ch],1e-5f));
            it->second[(size_t)ch]=std::max(it->second[(size_t)ch],db);
        }
    }
    if(g_mixedPd)for(const auto& track:g_apiProc.takePdPeaks()) {
        auto it=g_pdMeterDb.find(track.trackId);
        if(it==g_pdMeterDb.end())it=g_pdMeterDb.emplace(track.trackId,PdMeterArr{}).first;
        for(int ch=0;ch<(int)track.value.size();++ch) {
            const float db=20.f*std::log10(std::max(track.value[(size_t)ch],1e-5f));
            it->second[(size_t)ch]=std::max(it->second[(size_t)ch],db);
        }
    }
    g_monAge += dt;
    g_rawAge += dt;
    if (g_monAge > 0.5f) std::fill(g_peakIn.begin(), g_peakIn.end(), 0.f);
    if (g_meterDb.size() != g_peakIn.size()) g_meterDb.assign(g_peakIn.size(), -90.f);
    if (g_meterClipHold.size() != g_peakIn.size()) g_meterClipHold.assign(g_peakIn.size(), 0.f);
    for (size_t i = 0; i < g_peakIn.size(); ++i) {
        const float d = 20.f * std::log10(std::max(g_peakIn[i], 1e-5f));
        g_meterDb[i] = std::max(d, g_meterDb[i] - 36.f * dt);
        g_meterClipHold[i] = std::max(0.f, g_meterClipHold[i] - dt);
    }
}
#include "app.h"
bool engineIsUp() { return g_liveCsound && !g_playWaiting && (g_useCsoundApi ? g_apiProc.running() : g_playProc.running()); }
#include "app.h"
std::vector<VstJob> collectVstJobs(const Project& project, const std::vector<LaneMeterInfo>& meters, bool ignoreMuteSolo )
{
    std::vector<VstJob> jobs;
    // Same mute/solo rule as the Csound, PD and SC engines: a muted track is silent, and when any
    // track is soloed only the soloed tracks sound.
    bool anySolo = false;
    for (const Track& t : project.tracks) if (t.solo && !t.muted) anySolo = true;
    for (const Track& t : project.tracks) {
        if ((t.kind != 3 && t.kind != 4) || t.vstPath.empty()) continue;
        if (!ignoreMuteSolo && (t.muted || (anySolo && !t.solo))) continue;
        if (trackFrozen(t)) continue;   // frozen: no plugin instance runs for this track (the frozen WAV plays instead)
        VstJob j; j.id = t.id; j.name = vstBusName(t); j.path = t.vstPath; j.classId = t.vstClassId;
        j.parameterState = t.vstParameterState;
        j.stateBlob = vstui::editorStateBlob(t.id);   // SEPARATED ENGINES: the editor instance's loaded patch (Kontakt .nki) - empty until captured
        if (j.stateBlob.empty()) j.stateBlob = t.vstPatchState;   // a project-saved patch state (v58) starts the fresh instance with the same library
        j.instrument = t.kind == 3; j.pick = t.vstPick; j.init = t.vstInit;
        j.trackVolume = std::max(0.f, std::min(kMaxVolumeGain, t.volume));   // the fader rides the output at the master join
        j.outMode = t.vstOutMode == 1 ? 1 : 0;
        j.channels = std::max(1, std::min(8, t.vstTrackChannels()));   // the track's channel format (set by the user)
        j.outWide = j.channels > 2 ? 1 : 0;   // legacy field: wider than stereo = channels 1:1
        for (int k = 0; k < 8; ++k) j.slotStage[(size_t)k] = t.kind == 4 ? std::min(7, std::max(0, t.vstSlotStage[k])) : 0;
        j.outMap = Track::routeOutMap(t.vstRouteCode, j.outMapOn);   // the routing text (out<n> = in<m>), like a sample clip
        j.outMap = Track::routeOutMap(t.vstRouteCode, j.outMapOn);   // the routing text (out<n> = in<m>), like a sample clip
        j.outMap = Track::routeOutMap(t.vstRouteCode, j.outMapOn);   // the routing text (out<n> = in<m>), like a sample clip
        j.outMap = Track::routeOutMap(t.vstRouteCode, j.outMapOn);   // the routing text (out<n> = in<m>), like a sample clip
        j.outMap = Track::routeOutMap(t.vstRouteCode, j.outMapOn);   // the routing text (out<n> = in<m>), like a sample clip
        j.chain.clear();
        for (int st = 1; st < 8 && t.kind == 4; ++st) {   // the effect chain: stages 2..8, contiguous (VSTe only)
            const Track::VstStage& sg = t.vstChain[st - 1];
            if (sg.path.empty()) break;
            VstJob::ChainStage cs;
            cs.path = sg.path; cs.classId = sg.classId; cs.parameterState = sg.parameterState;
            cs.stateBlob = vstui::editorStateBlob(t.id, st);   // the editor instance's loaded patch, per stage
            if (cs.stateBlob.empty()) cs.stateBlob = sg.patchState;   // a project-saved patch state starts the fresh instance with the same library
            j.chain.push_back(std::move(cs));
        }
        j.bypass = (t.kind == 4 && t.vstBypass0) ? 1u : 0u;   // effect on/off: bit k = effect k+1 bypassed
        for (int st = 1; st < 8 && t.kind == 4; ++st) if (!t.vstChain[st - 1].path.empty() && t.vstChain[st - 1].bypass) j.bypass |= 1u << st;
        bool anyIn = false;
        for (int s = 0; s < 8; ++s) {
            j.inputs[s].on = t.vstInputs[s].on; j.inputs[s].mode = t.vstInputs[s].mode; j.inputs[s].name = t.vstInputs[s].name; j.inputs[s].sidechain = t.vstInputs[s].sidechain;
            j.inputs[s].receive = t.vstInputs[s].receive;
            const bool configured = t.vstInputs[s].mode == 2 || ((t.vstInputs[s].mode == 0 || t.vstInputs[s].mode == 1) && !t.vstInputs[s].name.empty());
            if (configured) anyIn = true;
        }
        j.routedIn = !j.instrument && anyIn;
        // PD / SuperCollider clips are rendered outside Csound, so their audio never reaches the
        // cawvstin channels. VSTi / VSTe tracks are the same: the rack renders their plugins and
        // adds the result straight to the master. Those sources feed the WHOLE source mix into
        // the slot instead (stereo - the same channels the source sends to the master). Same
        // lookup order as resolveVstInputRoutes: PD / SC first, then VST, then Csound lines.
        const auto engineOf = [&project](const std::string& trackName, const std::string& clipName, int mode, int& srcId) {
            for (const Track& o : project.tracks) {
                if (!trackFrozen(o)) continue;
                const bool match = mode == 0 ? o.name == trackName
                    : std::any_of(o.clips.begin(), o.clips.end(),
                        [&clipName](const Clip& c) { return c.name == clipName; });
                if (match) { srcId = o.id; return 4; }
            }
            const auto owner = [&](int kindA, int kindB) -> const Track* {
                for (const Track& o : project.tracks) {
                    if (o.kind != kindA && o.kind != kindB) continue;
                    const bool match = mode == 0 ? o.name == trackName
                        : std::any_of(o.clips.begin(), o.clips.end(),
                            [&clipName](const Clip& c) { return c.name == clipName; });
                    if (match) return &o;
                }
                return nullptr;
            };
            // A FROZEN source track plays its rendered WAV through the sample engine (one WAV voice
            // per track): its sound reaches the rack through the same per-track sample ring the
            // sample tracks use (engine 4) - its own engine no longer runs, so its old route (PD / SC
            // mix, VST output or Csound taps) is silent.
            if (const Track* o = owner(5, 6)) {
                if (trackFrozen(*o)) { srcId = o->id; return 4; }                    // frozen: the WAV voice rides the sample-engine ring
                srcId = o->kind == 6 ? o->id : scTrackSlot(o->id);                   // SC: srcId = the track slot (the per-track SC ring's key)
                return o->kind == 6 ? 1 : 2;
            }
            if (const Track* o = owner(3, 4)) { srcId = o->id; return trackFrozen(*o) ? 4 : 3; }                       // VSTi / VSTe output, wired by the rack
            if (const Track* o = owner(7, 7)) { srcId = o->id; return 4; }                      // sample track: its stereo mix via the sample-engine ring
            return 0;                                                                           // a Csound track: the CSD routing writes the lines
        };
        // Carry the resolved line ranges into the job so the rack can pack the ENABLED slots from
        // line 1 (live on/off never leaves silent gaps in front of a later input).
        if (j.routedIn) {
            const auto routes = resolveVstInputRoutes(project, t);
            for (const VstInputRoute& r : routes) {
                if (r.slot < 0 || r.slot >= 8) continue;
                j.inputs[r.slot].lineStart = r.lineStart;
                j.inputs[r.slot].lineCount = r.lineCount;
                if (r.mode != 2) j.inputs[r.slot].engine = engineOf(r.name, r.name, r.mode, j.inputs[r.slot].srcId);
                if (j.inputs[r.slot].engine == 0) j.inputs[r.slot].csChans = r.chans;   // the per-clip tap channels the CSD publishes
            }
        }
        for (const LaneMeterInfo& lm : meters) if (lm.trackId == t.id) { j.meterFirst = lm.first; j.meterCount = lm.channels; break; }
        jobs.push_back(j);
    }
    return jobs;
}
#include "app.h"
std::vector<VstJob> collectVstEngineJobs() {
    return collectVstJobs(g_proj, {}, true);   // muted or not: M / S are applied live
}
#include "app.h"
std::vector<std::string> collectVstTapChannels(const std::vector<VstJob>& jobs) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const VstJob& j : jobs)
        for (const auto& in : j.inputs) {
            if (!in.on || in.engine != 0) continue;
            for (const std::string& c : in.csChans)
                if (seen.insert(c).second) out.push_back(c);
        }
    return out;
}
#include "app.h"
void pushVstTapsToCsound() {
    g_apiProc.setVstTaps(collectVstTapChannels(collectVstJobs(g_proj, g_engineMeters, true)));
}
#include "app.h"
std::vector<VstSoloNote> buildVstSoloNotes() {
    std::vector<VstSoloNote> out;
    bool anySolo = false;
    for (const Track& t : g_proj.tracks) if (t.solo && !t.muted) anySolo = true;
    for (const Track& t : g_proj.tracks) {
        if (t.muted || (anySolo && !t.solo) || t.midiClips.empty()) continue;
        if (trackFrozen(t)) continue;   // frozen: the live engines skip the track (its notes are baked into the WAV)
        for (const MidiClip& mc : t.midiClips) {
            if (mc.muted) continue;
            int targetTrackId = -1;
            if (t.kind == 3 || t.kind == 4) {
                targetTrackId = t.id;   // VSTi or VSTe: MIDI clips drive the plugin directly
            } else {
                int laneIdx = -1;
                const Clip* target = findClip(mc.clipId, &laneIdx);
                if (!target || target->muted || laneIdx < 0) continue;
                const Track& vt = g_proj.tracks[(size_t)laneIdx];
                if ((vt.kind != 3 && vt.kind != 4) || vt.muted || (anySolo && !vt.solo) || trackFrozen(vt)) continue;   // not a VST track: the Csound path plays it
                targetTrackId = vt.id;
            }
            for (const MidiNote& n : mc.notes) {
                if (n.start >= mc.dur) continue;
                VstSoloNote sn;
                sn.start = mc.start + n.start;
                sn.dur = std::max(0.005, std::min(mc.start + mc.dur, sn.start + n.dur) - sn.start);
                sn.pitch = std::min(127, std::max(0, n.pitch));
                sn.velocity = std::min(127, std::max(1, n.velocity));
                sn.trackId = targetTrackId;
                if (targetTrackId >= 0) out.push_back(sn);
            }
        }
    }
    return out;
}
#include "app.h"
std::vector<VstScript> buildVstScripts() { return buildVstScripts(g_proj); }
// The render-copy overload: an offline render (freeze / export part) passes its stripped copy, so
// only the track being rendered contributes scripts and its solo/audible state counts.
std::vector<VstScript> buildVstScripts(const Project& proj) {
    std::vector<VstScript> out;
    bool anySolo = false;
    for (const Track& t : proj.tracks) if (t.solo && !t.muted) anySolo = true;
    for (const Track& t : proj.tracks) {
        if ((t.kind != 3 && t.kind != 4) || t.muted || (anySolo && !t.solo)) continue;   // VSTi (3) AND VSTe (4): the clip code runs in C++ here
        if (&proj == &g_proj && trackFrozen(t)) continue;   // frozen: the clip code does not run (the WAV carries the sound)
        for (const Clip& c : t.clips) {
            if (c.muted || c.code.empty()) continue;
            VstScript s;
            s.trackId = t.id;
            s.clipId = c.id;
            s.start = c.start;
            s.dur = std::max(0.01, c.dur);
            s.code = c.code;
            for (int k = 0; k < 8; ++k) {
                double v = k < (int)t.vstInit.size() ? t.vstInit[(size_t)k] : 0.5;
                s.params[k] = std::isfinite(v) ? std::min(1.0, std::max(0.0, v)) : 0.5;
            }
            // Bind each slot to its timeline automation lane (vstPickName) so the C++ interpreter
            // drives the slots the clip code does not write - the drawn ramps, no Csound.
            for (int k = 0; k < 8; ++k) {
                s.pickName[k] = k < (int)t.vstPickName.size() ? t.vstPickName[(size_t)k] : std::string();
                if (!s.pickName[k].empty()) {
                    auto it = t.automation.find(s.pickName[k]);
                    if (it != t.automation.end())
                        for (const AutoPoint& ap : it->second) s.autoPts[k].push_back({ap.t, ap.v});
                }
            }
            out.push_back(std::move(s));
        }
    }
    return out;
}
#include "app.h"
void ensureVstSoloOutput() {
    // The stream opens as soon as the VST engine runs - even before a plugin is loaded: the
    // sample engine (and the idle rack) render through it, so samples are heard at once.
    if (!g_vstEngineOn || g_vstSoloOutput || g_vstRetryT > 0.0) return;
    // SEPARATED ENGINES: the 4th engine owns the VST graph in EVERY mode. API Csound (whose stream
    // mixes this engine via renderMix), PD and SC block a dedicated stream here; the command-line
    // Csound opens ITS OWN device, so the VST engine still needs one in that mode.
    if (scHasSharedAudioOwner() || g_scSoloOutput || g_sampleOutput || g_testSound.running() || g_renderActive) return;
    // The dedicated Pure Data engine's stream is the carrier: the rack is mixed into it through
    // the shared realtime hook (scRealtimeSource). Opening a second stream here rendered the
    // rack a SECOND time per callback period and double-drained the PD mix ring - the rack
    // starved and a PureData input went silent while PD ran.
    if (g_pdEngineOn && g_pdTimelineEngine.active()) return;
    if (g_useCsoundApi && g_apiProc.active()) return;   // the Csound API engine's stream is the carrier: the rack is mixed into it
    if (!g_audioDevicesReady) refreshAudioDevices(false);
    if (g_proj.cs.outDev == -2) return;   // no output chosen: Settings already asks for one
    CawAudioSettings settings = pdAudioSettings();
    std::string error;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt == 1) {   // the selected device failed: re-enumerate, then fall back to the default output
            refreshAudioDevices(true);
            settings = pdAudioSettings();
            if (settings.deviceLabel == "<selected device missing>") settings.deviceLabel.clear();   // empty label = PortAudio default
        }
        auto output = std::make_unique<CawAudioOutput>();
        auto capture = std::make_unique<MonitorCapture>(settings.sampleRate, settings.channels);
        output->setMonitorCapture(capture.get());
        // settings keeps its realtime hook (scRealtimeSource): it mixes the VST rack into the device
        // AND renders the sample engine (audibly, and into the VSTe-input rings) while its own
        // stream is closed - one stream owns the card, both engines are heard.
        if (output->open(settings, error)) {
            closeSampleOutput();   // success only: one renderer - the sample engine renders in THIS stream now (a failed open must never kill the sample stream)
            clearSampleRings();    // the rings hold audio from the sample engine's own stream era: drop it or the effect stays behind
            output->setExpectingAudio(false);
            g_vstSoloCapture = std::move(capture);
            g_vstSoloOutput = std::move(output);
            g_vstSoloSnapshotSeq = 0;
            logLine("[caw] VST engine: playing through " + g_vstSoloOutput->description());
            return;
        }
    }
    logLine("[caw] VST engine output: " + error + " (the selected device and the default both failed) - retrying in 2 s.");
    g_vstRetryT = 2.0;   // retry quickly: while no stream carries audio, the 4th engine cannot render at all
}
#include "app.h"
std::set<int> collectVstActiveTracks(const Project& project) {
    std::set<int> ids;
    bool anySolo = false;
    for (const Track& t : project.tracks) if (t.solo && !t.muted) anySolo = true;
    // The normal rule: an unmuted track sounds on its own (or when any solo lets it).
    for (const Track& t : project.tracks)
        if ((t.kind == 3 || t.kind == 4) && !t.vstPath.empty() && !t.muted && !trackFrozen(t) && (!anySolo || t.solo))
            ids.insert(t.id);   // frozen: no plugin instance for this track (the frozen WAV plays instead)
    // An EFFECT wired to an audible source keeps processing it even while ANOTHER track is soloed:
    // it is that source's insert, so it belongs to the source's signal path - exactly like the
    // master FX chain, which the track M / S never touches. Without this, soloing the source track
    // (the natural way to audition it) silently skipped every VSTe wired to it: the source played
    // dry and the plugin "did nothing". Repeated so an effect chain follows its sources.
    const auto srcAudible = [&](const Track& o) {
        if (o.muted) return false;
        if (o.kind == 3 || o.kind == 4) return ids.count(o.id) != 0;   // a VST source follows its own chain
        return !anySolo || o.solo;   // a frozen source is audible through its sample-engine WAV voice
    };
    bool grew = true;
    while (grew) {
        grew = false;
        for (const Track& t : project.tracks) {
            if (t.kind != 4 || t.vstPath.empty() || t.muted || ids.count(t.id)) continue;
            bool follows = false;
            for (int s = 0; s < 8 && !follows; ++s) {
                const auto& in = t.vstInputs[s];
                if (!in.on || in.mode == 2) continue;
                for (const Track& o : project.tracks) {
                    if (o.id == t.id) continue;
                    const bool match = in.mode == 0 ? o.name == in.name
                        : std::any_of(o.clips.begin(), o.clips.end(),
                            [&](const Clip& c) { return c.name == in.name; });
                    if (match && srcAudible(o)) { follows = true; break; }
                }
            }
            if (follows) { ids.insert(t.id); grew = true; }
        }
    }
    return ids;
}
#include "app.h"
// Sample clips and every locked source reach a VSTe through the sample engine's per-track rings.
static bool vstInputsNeedSampleEngine() {
    for (const Track& vt : g_proj.tracks) {
        if (vt.kind != 4) continue;
        for (const auto& in : vt.vstInputs) {
            if (!in.on || in.mode == 2 || in.name.empty()) continue;
            for (const Track& o : g_proj.tracks) {
                if (o.kind != 7 && !trackFrozen(o)) continue;
                if (trackFrozen(o) && o.freezePath.empty()) continue;
                const bool match = in.mode == 0 ? o.name == in.name
                    : std::any_of(o.clips.begin(), o.clips.end(), [&](const Clip& c) { return c.name == in.name; });
                if (match) return true;
            }
        }
    }
    return false;
}

void ensureSampleEngineForVstInputs() {
    static bool warnedAudioOff = false;
    if (!vstInputsNeedSampleEngine()) { warnedAudioOff = false; return; }
    if (!g_langAudio) {
        if (!warnedAudioOff)
            logLine("[caw] VSTe input problem: a sample or locked source feeds an effect, but Audio is disabled in Settings > Engines.");
        warnedAudioOff = true;
        return;
    }
    warnedAudioOff = false;
    if (!g_sampleEngineOn) {
        g_sampleEngineOn = true;
        ensureSampleOutput();
        logLine("[caw] Sample engine started automatically: a sample or locked source feeds a VSTe input.");
    }
}

void pushVstActiveLive() {
    ensureSampleEngineForVstInputs();
    if (g_liveCsound && g_useCsoundApi) {
        g_apiProc.setVstActiveTracks(collectVstActiveTracks(g_proj));
        g_apiProc.setControlChannels(vstGateValues());   // M / S move which effect replaces its sources (FX only keeps consuming while its rack is muted)
        pushVstTapsToCsound();   // freeze / unfreeze can change a source from the sample ring to a Csound tap
        // Live plugins belong only to the VST engine; the runner's rack is offline-only.
    }
    if (g_vstEngineOn) {
        vstSolo().setActive(vstSoloActiveIds());
        vstSolo().setWiring(collectVstEngineJobs());   // same re-route for the 4th engine's rack
    }
    updateScSlotSuppress();   // M / S move which SC track slots an FX-only VSTe consumes (their raw mix stops reaching the master)
}
#include "app.h"
void vstEngineJoinTransport() {   // notes + loop + position, from the current playhead
    // SEPARATED ENGINES: the 4th engine hosts the VST graph in every mode, so IT gets the schedule
    // pushed here (the CSD's CawVstNote helper instruments are dead code while separated - nothing
    // consumes their note rings any more).
    const std::vector<VstSoloNote> notes = buildVstSoloNotes();
    if (!g_vstEngineOn) return;
    vstSolo().setNotes(notes);
    vstSolo().setLoop(timeline_loop::valid(g_proj), g_proj.loopStart, g_proj.loopEnd);
    vstSolo().play(g_play);
}
#include "app.h"
bool applyVstInstrumentJobs(const std::vector<VstJob>& jobs) {   // (re)load the plugins when the set changed
    vstui::captureParameters(g_proj);
#if defined(CAW_VST3) && defined(_WIN32)
    static bool com = false; if (!com) { OleInitialize(nullptr); com = true; }   // plugins expect COM on the thread that loads them
#endif
    const CawAudioSettings audio = pdAudioSettings();
    static VstEngineAudio soloAudio;   // the SuperCollider per-track rings are captured by CAW's callback (scTrackRingOf)
    soloAudio.scRing = &g_scRing;
    soloAudio.scTrackRing = [](int slot) -> CawScRing* { return scTrackRingOf(slot); };
    soloAudio.pdMix = nullptr;         // the mixed-mode PD mix lives inside the Csound runner (this engine runs while Csound is off)
    soloAudio.pdRing = &pdlivering::ring;   // the dedicated PD engine's live mix: a PureData input works WITHOUT Csound
    soloAudio.sampleRing = [](int trackId) -> CawScRing* { return sampleRingOf(trackId); };   // a sample track's stereo mix (engine 4)
    if (!vstSolo().start(jobs, audio.sampleRate, std::max(1, std::min(8, audio.channels)), 512, &soloAudio)) return false;   // the audio setting's width
    clearSampleRings();   // a (re)loaded graph starts from "now": stale ring audio would put the effect seconds behind
    vstSolo().setActive(vstSoloActiveIds());
    vstSolo().setInputMasks(collectVstInputMasks(g_proj));
    if (!g_liveCsound && !g_playing) {   // the effects run in THIS engine: say what each one can hear here
        for (const VstJob& j : jobs) {
            if (j.instrument) continue;
            std::string sum;
            bool csInput = false;
            for (int s = 0; s < 8; ++s) {
                const auto& src = j.inputs[s];
                if (!src.on || src.lineCount <= 0) continue;
                if (!sum.empty()) sum += ", ";
                sum += "in" + std::to_string(s + 1) + ": ";
                if (src.mode == 2) sum += "master";
                else if (src.engine == 1) { sum += "\"" + src.name + "\" (PureData mix)"; }
                else if (src.engine == 2) sum += "\"" + src.name + "\" (SuperCollider mix)";
                else if (src.engine == 3) sum += "\"" + src.name + "\" (VST output)";
                else if (src.engine == 4) sum += "\"" + src.name + "\" (sample engine: sample track or rendered WAV)";
                else { sum += "\"" + src.name + "\" (Csound track - needs Csound)"; csInput = true; }
            }
            logLine("[caw] VSTe \"" + j.name + "\" runs in the VST engine; inputs: " +
                    (sum.empty() ? std::string("(none enabled - nothing to process)") : sum) + ".");
            if (csInput)
                logLine("[caw] VSTe \"" + j.name + "\": a Csound track only sounds while Csound runs. "
                        "With the VST engine alone, wire the effect to a VSTi track, the master, a PureData track, or the SuperCollider mix.");
        }
    }
    return true;
}
#include "app.h"
void vstEngineAfterLoad() {
    vstSolo().setScripts(buildVstScripts());   // clip code ALWAYS runs here, in C++ - separated from Csound
    vstSolo().setWiring(collectVstEngineJobs());   // the effect inputs / output modes as configured now
    if (g_timelineRun) vstEngineJoinTransport();   // Play was already running: join it from the playhead
    else { vstSolo().setNotes(buildVstSoloNotes()); vstSolo().setLoop(timeline_loop::valid(g_proj), g_proj.loopStart, g_proj.loopEnd); }
    ensureVstSoloOutput();   // its own stream, unless Csound / PD / SC already carry the audio
}
#include "app.h"
bool vstEngineLoadJobs(const std::vector<VstJob>& jobs) {
    if (applyVstInstrumentJobs(jobs)) {
        g_vstFailedSig.clear(); g_vstFailedRetryT = 0.0;
        g_vstIdleNoPlugin = false;   // a plugin is in: the engine is really ready now
        vstEngineAfterLoad();
        return true;
    }
    g_vstFailedSig = VstSoloEngine::signature(jobs);
    g_vstFailedRetryT = 5.0;
    return false;
}
#include "app.h"
void startVstEngine() {
    if (g_vstEngineOn) return;
    if (!g_langVst) {   // the session list says "do not load VST": tracks and their code still work
        playNotice("VST is not enabled for this session - enable it in Settings > Engines.", "VST is disabled");
        logLine("[caw] VST is not enabled (Settings > Engines): Run VST does nothing, the plugins are not loaded.");
        return;
    }
    if (g_renderActive) { playNotice("An export is running - wait for it or cancel it first.", "Export running"); return; }
    if (!g_audioDevicesReady) refreshAudioDevices(false);
    if (g_proj.cs.outDev == -2) { playNotice("Select an audio output in Settings > Audio.", "Select an audio output"); return; }
    const std::vector<VstJob> jobs = collectVstEngineJobs();
    wireVstUiHooks();
    vstSolo().setLog([](const std::string& line) {   // called from the audio thread too
        std::lock_guard<std::mutex> lock(g_vstLogMutex);
        if (g_vstLogQueue.size() < 200) g_vstLogQueue.push_back(line);
    });
    // The engine runs even with no plugin yet: choosing / changing a .vst3 later loads it
    // automatically (notifyPluginLoaded + vstEngineCheckJobs) without a Stop / Run VST cycle -
    // and the P1..P8 mapping is carried over to the new plugin.
    g_vstEngineOn = true;
    ensureSampleEngineForVstInputs();
    g_vstIdleNoPlugin = jobs.empty();   // nothing picked yet: the pill must not wait for a load that is not running
    // Editor-side VST3 instances may have been opened before Run VST. Once the engine format is
    // known, negotiate it with those instances too (without starting a second audio processor).
    vstui::prepareAllEditorPlugins(g_proj, pdAudioSettings().sampleRate, 512);   // SEPARATED: device format - the editor instances match the VST engine, never Csound's ksmps
    g_vstRetryT = 0.0;
    g_vstFailedSig.clear();
    g_vstFailedRetryT = 0.0;
    g_vstJobsT = 0.0;
    g_vstMeterDb.clear();
    const bool kept = vstSolo().ready();   // Stop VST keeps the plugins loaded: this Run resumes them
    if (kept && jobs.empty()) vstSolo().unload();   // the plugin was removed while stopped: free its instances
    vstSolo().setEngineOn(true);
    if (jobs.empty()) {
        playNotice("The VST engine is on. Load a .vst3 into a VST track (right-click the track) - it starts by itself.",
                   "DSP VST: waiting for a plugin");
        vstEngineAfterLoad();
    } else if (!vstEngineLoadJobs(jobs)) {
        playNotice("The VST engine is on, but the plugin did not load - see the log. It is retried every 5 s (picking the plugin again retries at once).",
                   "VST engine: no plugin loaded - see the log");
    } else if (kept) {
        logLine("[caw] VST engine: the plugins stayed loaded - the same instances play again (no reload).");
    }
    logLine("[caw] VST engine on: VSTi play from their MIDI clips and clip code; VSTe effects run here while Csound is off. Press Play.");
}
#include "app.h"
void stopVstEngine(bool unloadPlugins) {
    if (!g_vstEngineOn) {
        if (unloadPlugins && vstSolo().ready()) {   // stopped earlier with the plugins kept: free them now
            vstSolo().unload();
            logLine("[caw] VST engine stopped, plugins unloaded.");
        }
        return;
    }
    g_vstEngineOn = false;
    g_vstIdleNoPlugin = false;
    g_vstFailedRetryT = 0.0;
    g_vstJobsT = 0.0;
    vstSolo().setEngineOn(false);
    if (g_vstSoloOutput) { g_vstSoloOutput->close(); g_vstSoloOutput.reset(); }
    g_vstSoloCapture.reset();
    g_vstSoloSnapshotSeq = 0;
    g_vstMeterDb.clear();
    if (unloadPlugins) {
        vstSolo().unload();   // a new project / exit: the plugins go away with it
        logLine("[caw] VST engine stopped, plugins unloaded.");
    } else {
        // Stop VST only stops the sound: the plugin instances stay loaded so Run VST resumes
        // exactly what was playing. Unloading and re-creating them here is what left an
        // instrument silent until its .vst3 was picked again.
        vstSolo().stop();
        logLine("[caw] VST engine stopped (the plugins stay loaded - Run VST resumes them).");
    }
}
#include "app.h"
void vstEngineCheckJobs(float dt) {
    if (!g_vstEngineOn) return;
    g_vstJobsT += dt;
    if (g_vstJobsT < 0.5) return;
    g_vstJobsT = 0.0;
    {   // SUPERCOLLIDER-STYLE: the clip code is pushed ONLY on Alt+Enter / Run (applyClipCodeNow) and once
        // when the engine starts (vstEngineAfterLoad). It is NOT rebuilt every 0.5 s here - that would
        // apply edits while typing. This block now only logs the mode once.
        static bool scriptsLogged = false;
        if (!scriptsLogged) { scriptsLogged = true; logLine("[caw] VST engine: the clip code (note / cc / msg, P1..P8) runs in C++ here, applied with Alt+Enter / Run."); }
    }
    pushVstActiveLive();   // whichever engine hosts the VST graph follows the live M / S state
    const std::vector<VstJob> jobs = collectVstEngineJobs();
    vstSolo().setWiring(jobs);   // effect inputs / output modes as configured now (no plugin reload)
    vstSolo().refreshLatencies();   // the rack loads async: keep the reported latencies current (dry/wet alignment)
    static bool retainedForLock = false;
    if (!jobs.empty()) retainedForLock = false;
    if (vstSolo().sameJobs(jobs)) { g_vstFailedSig.clear(); return; }
    if (jobs.empty()) {
        g_vstFailedSig.clear();
        const bool lockedVstRemains = std::any_of(g_proj.tracks.begin(), g_proj.tracks.end(), [](const Track& t) {
            return (t.kind == 3 || t.kind == 4) && !t.vstPath.empty() && trackFrozen(t);
        });
        if (lockedVstRemains) {
            // Keep the native instance alive: some VST3 factories fault when a just-bounced effect is destroyed.
            if (!retainedForLock) {
                vstSolo().setActive({});
                vstSolo().setWiring({});
                g_vstMeterDb.clear();
                logLine("[caw] VST engine: the remaining VST tracks are locked - keeping their plugin instances until unlock or project close.");
                retainedForLock = true;
            }
            return;
        }
        retainedForLock = false;
        if (vstSolo().ready()) {   // the last VST track / plugin went away: unload, keep the button on and wait
            vstSolo().unload();
            vstSolo().setEngineOn(true);   // Run VST stays on: a plugin loaded later starts playing by itself
            g_vstMeterDb.clear();
            logLine("[caw] VST engine: no plugin now - waiting (Run VST stays on).");
        }
        return;
    }
    retainedForLock = false;
    const std::string sig = VstSoloEngine::signature(jobs);
    if (sig == g_vstFailedSig && g_vstFailedRetryT > 0.0) return;   // this set already failed: retried when the timer runs out
    logLine("[caw] VST engine: the plugins changed - reloading.");
    if (!vstEngineLoadJobs(jobs))
        logLine("[caw] VST engine: the plugin could not be loaded - retrying every 5 s (see the lines above).");
}
#include "app.h"
void vstEngineTick(float dt) {
    {
        std::vector<std::string> queued;
        { std::lock_guard<std::mutex> lock(g_vstLogMutex); queued.swap(g_vstLogQueue); }
        for (std::string& line : queued) {
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            if (!line.empty()) logLine(line);
        }
    }
    if (!g_vstEngineOn) return;
    if (g_vstRetryT > 0.0) g_vstRetryT -= dt;
    if (g_vstFailedRetryT > 0.0) g_vstFailedRetryT -= dt;
    if (!g_vstSoloOutput) ensureVstSoloOutput();
    vstEngineCheckJobs(dt);
}
#include "app.h"
std::map<int, unsigned> collectVstInputMasks(const Project& project) {
    std::map<int, unsigned> masks;
    for (const Track& t : project.tracks) {
        if (t.kind != 4) continue;
        unsigned mask = 0;
        for (int s = 0; s < 8; ++s) if (t.vstInputs[s].on) mask |= (1u << s);
        masks[t.id] = mask;
    }
    return masks;
}
#include "app.h"
void commandStopAll() { sendCommand(true, row(12, 0, 0, 0, 0, 0), 1); }
#include "app.h"
void commandPlayFrom(double t, bool stopFirst) {
    std::string rows;
    int count = 0, sent = 0;
    if (stopFirst) { rows += row(12, 0, 0, 0, 0, 0); ++count; }
    const bool looping = timeline_loop::valid(g_proj);
    const double remaining = looping ? std::max(0.0, g_proj.loopEnd - t) : 0.0;
    for (TimelineEvent ev : timelineEvents(g_proj, t, stopFirst)) {
        if (looping && !timeline_loop::trim(ev, remaining)) continue;
        int type = ev.automation ? 11 : 10;
        int ix = 0;
        if (!ev.automation && ev.name.rfind("CawVstNote_", 0) == 0) {
            // SEPARATED ENGINES: Csound never hosts the VST graph, so its score never fires the
            // CawVstNote helper instruments - CAW's 4th engine schedules the MIDI-clip notes
            // itself (audio-rate, loop-aware; see vstEngineJoinTransport). Nothing to send here.
            continue;
        } else {
            ix = ev.automation ? numberOf(g_engChans, ev.name) : numberOf(g_engClips, ev.name);
        }
        if (!ix) { warnAddedAfterRun(); continue; }
        rows += row(type, ix, ev.start, ev.dur, ev.a, ev.b);
        ++count; ++sent;
    }
    if (looping) {
        rows += row(12, 0, remaining, 0, 0, 0);
        ++count;
    }
    sendCommand(true, rows, count);
    if (!g_engineSentShown && sent) { g_engineSentShown = true; logLine("[caw] timeline -> Csound: " + std::to_string(sent) + " clip / note / ramp starts from " + num(t) + " s"); }
}
#include "app.h"
void commandStartClip(const Clip& clip, double dur, double p4, double p5) {
    const int ix = numberOf(g_engClips, clip.name);
    if (!ix) { warnAddedAfterRun(); return; }
    sendCommand(false, row(10, ix, 0, dur, p4, p5), 1);
}
#include "app.h"
void commandStopClip(const Clip& clip) {
    const int ix = numberOf(g_engClips, clip.name);
    if (ix) sendCommand(false, row(13, ix, 0, 0, 0, 0), 1);
}
#include "app.h"
void stopPlayback() {
    if (g_useCsoundApi) g_apiProc.stop();
    else g_playProc.stop();
    drainPlayOutput();
    closeMonitorSocket();
    g_useCsoundApi = false;
    g_cawMonitorApi = false;
    g_monHeld.clear();
    g_playing = false;
    g_liveCsound = false;
    g_vstHostedInCsound = false;   // the Csound rack is gone: the 4th engine is the VST host again
    g_playWaiting = false;
    g_runLeft = 0.0;
    g_mixedPd = false;
    restoreScSoloOutput();   // SC and the VST engine get the soundcard back
}
#include "app.h"
void startEngine() {
    if (g_pdEngineOn || g_pdTimelineMode || g_pdPreview.active()) {
        // Csound + PD share one engine: stop the separate PD engine (and SC running on it) automatically, then continue.
        logLine("[caw] Stopping the separate PD engine; Csound + PD will run together on one engine.");
        if (g_scEngineOn) stopScEngine();
        if (g_pdTimelineMode && g_timelineRun) { pdTransport(false); g_timelineRun = false; }
        g_pdResumeAfterReload = false;
        g_pdPreview.stop();
        g_pdTimelineEngine.stop();
        g_pdPlayingClip = 0; g_pdEngineOn = false; g_pdTimelineMode = false; g_pdTimelineEvents.clear();
        restoreScSoloOutput();
    }
    if (g_testSound.running()) { logLine("[caw] Wait for the test sound to finish before starting Csound."); return; }
    if (g_renderActive) { logLine("[caw] A render is running - wait for it or cancel it first."); return; }
    if (!g_audioDevicesReady) refreshAudioDevices(false);
    if (g_playing) stopPlayback();
    vstui::captureParameters(g_proj);
    diagClearAll();
    g_cawScopeDiv = scopeDiv();
    g_cawScopePeak = g_scopeSample;
    g_scopeActiveMs = scopeWindowMs();
    g_scopeHasFrame = false;
    shareEngineDlls();
    // Hand the soundcard over: the API engine's stream becomes the carrier. Its open would fail
    // while CAW's VST / sample stream holds the device (exclusive drivers), and two CAW streams
    // would render the shared hook twice (engines at double speed). The stopped engines re-open
    // when Csound stops (restoreScSoloOutput).
    handAudioCardToNextStream();
    const std::string exe = csoundExePath();
    if (exe.empty()) {
        logLine("[caw] Csound was not found. Either install Csound (https://csound.com/download.html - Csound 6 and 7 both work) or copy a full portable Csound folder into CAW's folder as <CAW folder>\\Csound\\bin\\csound.exe (1-PREPARE.ps1 -CsoundDir <your Csound folder> stages it for you). CAW can also browse for it in Settings > Programs.");
        playNotice("Csound was not found - see the log for how to fix it.", "Csound missing");
        return;
    }
    if (!ensureCsoundMathProbe()) {
        // The probe result (and Csound's own message, if any) was just logged. Refuse only on real
        // evidence: a Csound that cannot compile abs OR sqrt in a tiny test cannot build CAW's meters.
        logLine("[caw] ERROR: " + exe + " cannot compile abs or sqrt (CAW compiled tiny test orchestras through the same Csound library the engine uses" + (g_mathProbeDetail.empty() ? ")." : ": " + g_mathProbeDetail + ").") + " CAW's meters need one of them; this Csound installation or copy is incomplete. Pick a full csound.exe in Settings > Programs (Browse opens CAW's own folder) or re-copy it with 1-PREPARE, then try again.");
        return;
    }
    std::string apiError;
    wireVstUiHooks();   // the panel's live-edit hooks (once is enough)
    g_useCsoundApi =!cawEnv("CAW_MON_TEXT") && !cawEnv("CAW_MON_OSC") && g_apiProc.prepare(exe, apiError);
    if (g_scEngineOn && !g_useCsoundApi) {
        logLine("[caw] SuperCollider can share output only with CAW's Csound API host. Clear CAW_MON_TEXT/CAW_MON_OSC or fix the Csound API setup.");
        g_useCsoundApi = false;
        return;
    }
    const bool mixedRequested = g_langCsound && g_langPd;
    std::vector<pd_timeline::Event> mixedPlan;
    if (mixedRequested) {
        if (!g_useCsoundApi) {
            logLine("[caw] Shared Csound/PD audio requires the Csound API host. " + apiError);
            return;
        }
        if (g_pdLibraryPath.empty()) g_pdLibraryPath = plat::bundledLibpd();
        if (g_pdLibraryPath.empty()) { logLine("[caw] libpd was not found. Select it in Settings > Programs, or copy libpd.dll (+ libwinpthread-1.dll) into <CAW folder>\\PureData\\."); return; }
        int bad=0;
        // Include muted clips, flagged: M / S inside the shared engine is a live replan, never
        // a "clips changed" restart of Csound + PD + the device.
        if (!pd_timeline::makePlan(g_proj,g_projectPath,mixedPlan,apiError,false,&bad,true)) {
            logLine("[caw] PD clip problem: "+apiError);
            if(bad>0)for(Track& t:g_proj.tracks)for(Clip& c:t.clips)if(c.id==bad)c.editorOpen=true;
            return;
        }
    }
    if (!g_useCsoundApi && std::fabs(g_proj.cs.masterGain - 1.f) > 0.0001f) {
        logLine("[caw] The master gain requires CAW host audio. Restore Master to 100% or enable the Csound API.");
        return;
    }
    g_cawMonitorApi = g_useCsoundApi;
    if (g_useCsoundApi) logLine("[caw] Using the Csound API and in-memory monitor.");
    else if (g_proj.cs.monitorFeed && !cawEnv("CAW_MON_TEXT")) {
        if (!apiError.empty()) logLine("[caw] " + apiError + " Trying the OSC monitor instead.");
        if (!openMonitorSocket()) logLine("[caw] OSC monitor unavailable, using the text monitor.");
        else logLine("[caw] Binary OSC monitor active (local only).");
    }
    const std::string csdPath = (workDir() / "caw_play.csd").string();
    Project engineProject = g_proj;
    if (!g_useCsoundApi) {
        // The legacy process engine uses Csound's own device indices.
        // Enumerate them only when that engine is actually needed.
        if (g_csoundOutDevs.empty() && g_csoundInDevs.empty()) refreshAudioDevices(true);
        // The old process engine opens the card by CSOUND's device number, which differs from the PortAudio list shown in Settings:
        // translate by device label (a device Csound's PortAudio cannot see falls back to the default).
        auto translate = [](int id, const std::vector<AudioDevice>& shown, const std::vector<AudioDevice>& csound, const char* what) {
            if (id < 0) return id;
            for (const AudioDevice& d : shown) if (d.id == id)
                for (const AudioDevice& c : csound)
                    if (normalizeCawDeviceLabel(c.name) == normalizeCawDeviceLabel(d.name)) return c.id;
            logLine(std::string("[caw] The selected ") + what + " device is not available in Csound's own audio system - using the default.");
            return -1;
        };
        engineProject.cs.outDev = translate(g_proj.cs.outDev, g_outDevs, g_csoundOutDevs, "output");
        engineProject.cs.inDev = translate(g_proj.cs.inDev, g_inDevs, g_csoundInDevs, "input");
        if (!engineProject.cs.options.empty()) syncOptionsFromSettings(engineProject.cs);
    }
    if (!writeTextFile(csdPath, buildCsd(engineProject, "engine", 0.0, &g_orcSpans))) {
        logLine("[caw] ERROR: could not write " + csdPath);
        closeMonitorSocket();
        g_cawMonitorApi = false;
        g_useCsoundApi = false;
        return;
    }
    const std::string cmd = "\"" + exe + "\" \"" + csdPath + "\"";
    if (!g_useCsoundApi) logLine("[caw] Run (Csound runs until Stop; Play sends the clips at their start times): " + cmd);
    resetCommands();   // the command files exist before Csound reads them
    g_engineMeters = meterLayout(g_proj, g_projectPath);
    g_recNames = cawWriteTargets(g_proj);
    int top = masterChannels(g_proj.cs);
    for (const LaneMeterInfo& lm : g_engineMeters) top = std::max(top, lm.first + lm.channels);
    {
        g_engineMaster = masterChannels(g_proj.cs);
        g_peakIn.assign((size_t)top, 0.f);
        g_meterDb.assign((size_t)top, -90.f);
        g_meterClipHold.assign((size_t)top, 0.f);
    }
    g_haveBands = false;
    g_haveRaw = false;
    g_scopeFreeze = false;
    g_scopeFrozenHasFrame = false;
    g_specFreeze = false;
    g_rawAge = 10.f;
    g_rawAssembler.reset();
    g_fftDb.clear();
    g_spectrumBands.clear();
    g_columnBands.clear();
    g_columnHold.clear();
    g_columnDisp.clear();
    g_tonePeaks.clear();
    monitor_peaks::resetTracking();
    g_fftHistory.clear();
    g_rawSampleRate = std::max(1, g_proj.cs.sr);
    g_rawFullScale = (float)std::max(0.0001, g_proj.cs.zdbfs);
    g_monHeld.clear();
    g_monSeen = false;
    g_monWarned = false;
    g_monRunT = 0.f;
    g_engineSentShown = false;
    g_apiSnapshotSeq = 0;
    if (g_useCsoundApi) {
        // First host-output version is output-only. Never silently drop a
        // configured recording input; the legacy OSC engine supports it.
        // The soundcard input is CAW-owned now (caw_audio_in.h): one capture stream for every engine.
        ensureCawAudioInput();
        if (g_proj.cs.outDev == -2) {
            logLine("[caw] No output selected. Choose a soundcard in Settings or use the OSC launcher for silent output.");
            g_cawMonitorApi = false; g_useCsoundApi = false;
            return;
        }
        CawAudioSettings audio;
        audio.sampleRate = g_proj.cs.sr;
        audio.channels = g_proj.cs.nchnls;
        audio.softwareBuffer = g_proj.cs.bufSW;
        audio.hardwareBuffer = g_proj.cs.bufHW;
        audio.fullScale = g_rawFullScale;
        attachScRealtimeSource(audio);
        int selected = g_proj.cs.outDev;
        if (selected == -1 && plat::isWindows) selected = pickDefaultDevice(g_outDevs, "WASAPI", false);
        if (selected >= 0) {
            audio.deviceLabel = "<selected device missing>";
            for (const auto& device : g_outDevs)
                if (device.id == selected) { audio.deviceLabel = device.name; break; }
        }
        {   // SEPARATED ENGINES: Csound renders ONLY its own instruments - it never loads or owns
            // VST plugins. The whole VST graph (instruments AND effects) lives in the independent
            // 4th engine (vst_solo.h) on its own thread and FIFO, so opening a plugin editor,
            // re-negotiating an instance or any plugin-side churn can never stall or race Csound's
            // audio. Csound clip taps (engine 0) still reach VSTe inputs: this worker captures the
            // named tap channels per ksmps cycle (setVstTaps) and the 4th engine's rack pops them.
            wireVstUiHooks();   // the panel edits reach the 4th engine's rack (wiring, output mode, plugin change)
            vstui::captureAllParameters(g_proj);   // Run bakes exactly what the plugin GUIs show right now
            // Every VST track is considered whatever its M / S state: mute / solo are applied live.
            std::vector<VstJob> rackJobs = collectVstJobs(g_proj, g_engineMeters, true);
            if (!g_langVst) rackJobs.clear();   // VST unchecked for the session: no taps either
            g_apiProc.setVstTaps(collectVstTapChannels(rackJobs));   // taps only - Csound loads NO VST plugins
            g_apiProc.setVstActiveTracks(collectVstActiveTracks(g_proj));   // initial live M / S state
            g_apiProc.setVstInputMasks(collectVstInputMasks(g_proj));       // initial live input on/off state
            // Source gates (cawvstmix_<clip>_<id>): 1 = that master-mode effect replaces the clip,
            // 0 = the clip reaches the master. The app keeps them current while the engine runs.
            g_apiProc.setControlChannels(vstGateValues());
            g_apiProc.setMasterGain(g_proj.cs.masterGain);
            g_apiProc.setPdSource(mixedRequested ? g_pdLibraryPath : std::string(),mixedPlan);
            // CAW hub (caws/cawr): the shared bus + the hub channel names found in the clip code.
            g_apiProc.setHub(&g_cawHub);
            g_apiProc.setHubNames(collectCawHubNames());
            { std::vector<std::string> bs, br; collectCawBusNames(bs, br); g_apiProc.setBusNames(bs, br); }   // audio buses (cawbsend / cawbrecv)
        }
        closeScSoloOutput();
        if (!g_apiProc.start(csdPath, g_rawSampleRate, top,
                             g_proj.cs.monitorFeed && !cawEnv("CAW_NO_MON"), apiError, &audio)) {
            restoreScSoloOutput();
            logLine("[caw] ERROR: " + apiError);
            g_cawMonitorApi = false; g_useCsoundApi = false;
            return;
        }
    } else {
        g_playProc.setHighPriority(true);
        if (!g_playProc.start(cmd, true)) {
            logLine("[caw] ERROR: " + g_playProc.lastError());
            closeMonitorSocket();
            return;
        }
    }
    g_playing = true;
    g_liveCsound = true;
    g_vstHostedInCsound = false;   // SEPARATED ENGINES: the 4th engine always owns the VST graph, Csound never does
    if (g_langVst && g_useCsoundApi && !g_vstEngineOn)
        startVstEngine();   // VST tracks keep sounding with Csound: the 4th engine starts with Run Csound, even with no plugin picked yet (it loads and starts by itself later)
    g_mixedPd = mixedRequested;
    if(g_mixedPd){g_pdTimelineEvents=std::move(mixedPlan);g_pdNeedsReload=false;g_pdPlanUpdateFailed=false;g_pdPlanT=0.0;}
    g_playWaiting = true;
    g_playWaitT = 0.f;
    g_sectionSeen = false;
    g_resync = 1;   // if the timeline is already playing, bring the new engine in sync with the playhead
}
#include "app.h"
bool replaceOutputFile(const std::string& temporary, const std::string& final, std::string& error) {
#ifdef _WIN32
    if (MoveFileExA(temporary.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    error = "Could not replace output file (Windows error " + std::to_string(GetLastError()) + ")";
    return false;
#else
    std::error_code ec;
    std::filesystem::rename(temporary, final, ec);
    if (!ec) return true;
    error = "Could not replace output file: " + ec.message();
    return false;
#endif
}
#include "app.h"
// The offline render carries the FROZEN tracks' sound through their rendered WAVs (the offline
// host decodes them and feeds them as engine-4 sample-ring inputs - see setOfflineSampleStreams).
// The live sample engine does not run offline, so these streams ARE the locked sources' voices.
std::map<int, CsoundApiRunner::OfflineSampleStream> collectOfflineSampleStreams(const Project& p);

void cleanupRenderTemps() {
    std::error_code ec;
    if (!g_renderTemp.empty()) std::filesystem::remove(g_renderTemp, ec);
    if (!g_renderEncodedTemp.empty()) std::filesystem::remove(g_renderEncodedTemp, ec);
    g_renderTemp.clear(); g_renderEncodedTemp.clear();
}
#include "app.h"
// ---- whole-project OFFLINE export (Render mode = offline): every engine renders through its own
// offline pipeline (the same per-track passes the freeze uses), one part per track, and the parts
// mix down into the single export file afterwards. A frozen track joins as its rendered WAV.
struct ExportOffline {
    bool active = false;                 // a queue is running (g_renderActive stays true for Cancel)
    std::vector<int> pending;            // track indexes still to render
    struct Part { std::string path; double t0; float gain = 1.f; };   // the WAV starts at timeline t0; gain = the track fader (parts render at unity, the mix applies it like the live mix does)
    std::vector<Part> parts;
    std::string finalPath, tempWav, encTmp;
    int format = 0, wavFormat = 0;
    int rate = 48000;
    double t0 = 0.0;                     // the export range start (the panel's Start)
    double t1 = 0.0;                     // the export range end (the panel's End, or the project end + tail)
    int partSeq = 0;                     // part file numbering
    std::chrono::steady_clock::time_point startedAt;
    bool asReal = false;                 // as real routing: the parts carry the whole virtual bus (settings N)
    int partCh = 2;                      // the channel width the parts were rendered at
    bool splitExport = false;            // each final channel exports as its own mono file
};
static ExportOffline g_exportOff;
static bool g_exportBeginPending = false;    // freezeTrackBegin is starting ON BEHALF of the export queue
static void exportNextTrack();   // defined below (drives the offline per-engine export queue)

// ---- real-time whole-project export (Settings > Audio: Render mode = Real-time capture)
// The transport plays once from the project start while the audio callback copies the FINAL
// master mix (every engine live, master FX included) into the export WAV. Works for every track
// kind - no offline pipeline needed - and uses the project's export format / bit depth / rate.
struct ExportRt {
    int phase = 0;                    // 0 idle, 1 capturing (transport rolling), 2 draining/writing, 3 encode handoff done
    double t0 = 0.0, t1 = 0.0;        // timeline extent being captured (t1 includes the tail)
    std::string tmpPath;
    int rate = 48000;
    int ch = 2;                       // the WAV's channels: the project's output channels (clamped 1..8)
    int wavFormat = 2;                // the WAV's sample format (float32 for the FLAC/MP3 handoff)
    double savedPlayhead = 0.0;
    bool savedLoopEnabled = false;
    double savedLoopStart = 0.0, savedLoopEnd = 0.0;
    FreezeCaptureSink sink;
    CawWavWriter wav;
    std::thread writer;
    std::atomic<bool> stopWriter{false};
    std::atomic<int> writeFailed{0};
    std::chrono::steady_clock::time_point startedAt;
    std::chrono::steady_clock::time_point captureStartedAt = std::chrono::steady_clock::now();   // reset on every playhead move (the stall watchdog)
    double lastPlay = 0.0;                                    // the last playhead seen (stall detection)
    std::string error;
    // Reset for a new pass (atomics cannot be assigned: every field is reset here; the caller
    // must have joined any previous writer thread and released the sink before).
    void resetForNewPass() {
        phase = 0;
        t0 = t1 = 0.0;
        tmpPath.clear();
        rate = 48000;
        savedPlayhead = 0.0;
        savedLoopEnabled = false;
        savedLoopStart = savedLoopEnd = 0.0;
        sink.quiesce();
        sink.reset();
        stopWriter.store(false, std::memory_order_release);
        writeFailed.store(0, std::memory_order_release);
        wavFormat = 2;
        error.clear();
    }
};
static ExportRt g_exportRt;
int activeStreamRate(int fallback);   // defined next to scheduleAudioTimeline

static void exportRtCancel() {
    if (g_exportRt.phase == 0) return;
    g_exportRt.sink.quiesce();
    g_exportSink.store(nullptr, std::memory_order_release);
    g_exportRt.stopWriter.store(true, std::memory_order_release);
    if (g_exportRt.writer.joinable()) g_exportRt.writer.join();
    if (timelinePlaying()) toggleTimelinePlay();
    g_play = g_exportRt.savedPlayhead;
    g_proj.loopEnabled = g_exportRt.savedLoopEnabled;
    g_proj.loopStart = g_exportRt.savedLoopStart;
    g_proj.loopEnd = g_exportRt.savedLoopEnd;
    g_exportRt.wav.close();
    std::error_code ec;
    if (!g_exportRt.tmpPath.empty()) std::filesystem::remove(std::filesystem::u8path(g_exportRt.tmpPath), ec);
    g_exportRt.phase = 0;
}

void startRenderRealtime(const std::string& renderPath, int format, int wavFormat) {
    double hi = 0.0;
    for (const Track& t : g_proj.tracks) {
        if (t.kind == 1) continue;
        for (const Clip& c : t.clips) hi = std::max(hi, c.start + c.dur);
        for (const MidiClip& mc : t.midiClips) hi = std::max(hi, mc.start + mc.dur);
        for (const auto& lane : t.automation)
            for (const AutoPoint& ap : lane.second) hi = std::max(hi, ap.t);
        // A locked track's sound runs to the END OF ITS RENDERED WAV (its clips no longer bound it):
        // the offline export must end where the real-time capture (and your ears) end.
        if (trackFrozen(t)) hi = std::max(hi, t.freezeEnd);
    }
    if (hi <= 0.0) { g_exportError = "The project has nothing to render (no clips)."; logLine("[caw] " + g_exportError); return; }
    const double extent = hi + std::max(0.0, g_proj.cs.tail) + 1.0;
    const double rangeStart = std::max(0.0, g_proj.cs.exportStart);
    const double rangeEnd = g_proj.cs.exportEnd > rangeStart ? std::min(g_proj.cs.exportEnd, extent) : extent;
    if (rangeEnd <= rangeStart) { g_exportError = "The export range is empty (End must be greater than Start)."; logLine("[caw] " + g_exportError); return; }
    const int projCh = std::max(1, std::min(8, masterChannels(g_proj.cs)));
    // The real-time export renders the LIVE mix: start every checked engine first (the DSP press
    // does the same), so the export works with the DSP off. The transport waits for them (arming).
    if (g_langCsound && !engineIsUp()) { logLine("[caw] Real-time export: starting Csound..."); startEngine(); }
    if (g_langSc && !g_scEngineOn) { logLine("[caw] Real-time export: starting SuperCollider..."); startScEngine(); }
    if (g_langVst && !g_vstEngineOn) { logLine("[caw] Real-time export: starting the VST engine..."); startVstEngine(); }
    if (g_langAudio && !g_sampleEngineOn) { g_sampleEngineOn = true; ensureSampleOutput(); logLine("[caw] Real-time export: the sample engine is on."); }
    const int fileCh = std::max(1, std::min(8, g_proj.cs.exportChannels));
    logLine("[caw] Real-time export: the file carries " + std::to_string(fileCh) + " channel(s) (the export panel's Channels setting; the project mix is " + std::to_string(projCh) + ").");
    g_exportRt.resetForNewPass();
    g_exportRt.t0 = rangeStart;
    g_exportRt.t1 = rangeEnd;
    g_exportRt.rate = activeStreamRate(std::max(8000, g_proj.cs.sr));   // provisional; re-read at arming, once the device rate is known
    g_exportRt.ch = std::max(1, std::min(8, g_proj.cs.exportChannels));   // the export panel's Channels setting
    g_exportRt.wavFormat = wavFormat;
    g_exportRt.tmpPath = renderPath;
    g_exportRt.savedPlayhead = g_play;
    g_exportRt.savedLoopEnabled = g_proj.loopEnabled;
    g_exportRt.savedLoopStart = g_proj.loopStart;
    g_exportRt.savedLoopEnd = g_proj.loopEnd;
    g_exportRt.stopWriter.store(false, std::memory_order_release);
    g_exportRt.writeFailed.store(0, std::memory_order_release);
    g_exportRt.sink.quiesce();
    g_exportRt.sink.reset();
    g_exportRt.sink.quiesce();
    g_exportRt.sink.reset();
    g_exportRt.startedAt = std::chrono::steady_clock::now();
    g_exportRt.phase = 4;   // arming: the transport waits until the started engines and the soundcard are up
    g_renderActive = true;
    logLine("[caw] Real-time export: waiting for the engines, then the transport plays " +
            std::to_string((int)(rangeEnd - rangeStart + 0.5)) + " s once and the final master mix is captured to " + renderPath +
            " (the WAV opens once the device rate is known).");
}

static void exportNextTrack();   // defined below (drives the offline per-engine export queue)

void startRenderNow() {
    if (g_renderActive) return;
    // The DSP switch decides the pipeline (user request): offline rendering must not run while the
    // DSP (any engine) is on, and real-time capture needs the engines of the checked languages up.
    // Both ask in a popup - never crash, never silently fail. The popup button performs the same
    // full start/stop as the DSP button, so the button state follows.
    if (g_proj.cs.renderMode == 0 &&
        (g_liveCsound || g_pdEngineOn || g_scEngineOn || g_vstEngineOn || g_sampleEngineOn)) { g_askDspOffExport = true; return; }
    if (g_proj.cs.renderMode == 1) {
        const bool dspUp = (!g_langCsound || g_liveCsound) && (!g_langPd || g_pdEngineOn || g_mixedPd) &&
                           (!g_langSc || g_scEngineOn) && (!g_langVst || g_vstEngineOn) && (!g_langAudio || g_sampleEngineOn);
        if (!dspUp) { g_askDspOnExport = true; return; }
    }
    if (freezeActive()) {   // a freeze render owns the offline host: starting an export would stop it mid-render
        g_exportError = "A track freeze is running. Wait for it to finish (or cancel it) before exporting.";
        logLine("[caw] " + g_exportError);
        return;
    }
    vstui::captureParameters(g_proj);
    g_exportError.clear();
    const int format = std::min(4, std::max(0, g_proj.cs.renderFormat));
    const int convertTarget = (g_proj.cs.exportConvertOn && g_proj.cs.exportConvert >= 1) ? g_proj.cs.exportConvert : 0;
    const int virtualBus = masterChannels(g_proj.cs);
    // The exported width: Convert wins (the fold happens at the writer); otherwise as real
    // routing exports the whole virtual bus (settings N) and as virtual follows the Channels combo.
    const int channels = convertTarget >= 1 ? convertTarget
        : (g_proj.cs.exportSource == 1 ? virtualBus : g_proj.cs.exportChannels);
    if (g_proj.cs.exportSplit && format >= 3) {
        g_exportError = "Channel-separated export writes WAV formats - pick WAV (PCM), or turn splitting off.";
        logLine("[caw] " + g_exportError);
        return;
    }
    if (g_proj.cs.renderMode == 1 && (g_proj.cs.exportSource == 1 || convertTarget >= 1)) {
        g_exportError = "The real-time capture always records the stereo master mix. For as real routing / Convert / separated channels use Render mode = Offline.";
        logLine("[caw] " + g_exportError);
        return;
    }
    if ((format == 4 && channels > 2) || (format == 3 && channels > 8)) {
        g_exportError = format == 4 ? "MP3 supports mono or stereo only." : "FLAC supports up to 8 channels.";
        logLine("[caw] " + g_exportError);
        return;
    }
    if (format == 4 && !cawMp3BitrateSupported(g_proj.cs.sr, g_proj.cs.exportMp3Kbps)) {
        g_exportError = "MP3 needs a supported sample rate (8 to 48 kHz) and bitrate. Check project audio settings.";
        logLine("[caw] " + g_exportError);
        return;
    }
    if (g_proj.cs.renderMode == 1) {
        // REAL-TIME EXPORT: every engine plays live and the final master mix is captured - all
        // track kinds (SC / PD / frozen / VST / sample), no Csound host, no offline pipelines.
        std::error_code ec;
        g_renderWillOverwrite = std::filesystem::exists(renderFilePath(), ec);
        if (ec) { g_exportError = "Could not check output: " + ec.message(); logLine("[caw] " + g_exportError); return; }
        for (const Track& track : g_proj.tracks)
            if (!track.muted && (track.kind == 3 || track.kind == 4) && !track.clips.empty() && track.vstPath.empty()) {
                g_exportError = "VST track " + track.name + " has clips but no plugin selected.";
                logLine("[caw] " + g_exportError);
                return;
            }
        if (g_playing) stopPlayback();
        const std::string renderPath = renderFilePath().string();
        g_renderPath = renderPath;
        g_activeRenderFormat = format;
        g_activeFlacBits = g_proj.cs.exportFlacBits;
        g_activeMp3Kbps = g_proj.cs.exportMp3Kbps;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path dir = std::filesystem::path(renderPath).parent_path();
        const std::string stem = ".caw_render_" + std::to_string(stamp);
        g_renderTemp = (dir / (stem + ".wav")).string();
        g_renderEncodedTemp = format >= 3 ? (dir / (stem + renderExtension(format))).string() : std::string();
        g_renderConverting = false;
        diagClearAll();
        startRenderRealtime(g_renderTemp, format, format >= 3 ? 2 : format);
        return;
    }
    // OFFLINE EXPORT, PER-ENGINE (no Csound requirement: each engine renders its own way - a
    // Csound/VSTi track renders through the offline Csound host, PD in CAW's C++ PD host,
    // SuperCollider through the NRT renderer, sample tracks in the C++ sample engine).
    if (g_pdEngineOn || g_pdPreview.active()) { logLine("[caw] Stop Pure Data playback before exporting."); return; }
    for (const Track& track : g_proj.tracks)
        if (!track.muted && (track.kind == 3 || track.kind == 4) && !track.clips.empty() && track.vstPath.empty()) {
            g_exportError = "VST track " + track.name + " has clips but no plugin selected.";
            logLine("[caw] " + g_exportError);
            return;
        }
    // OFFLINE EXPORT, PER-ENGINE: every audible track renders through its OWN offline pipeline
    // (the freeze passes: Csound host for Csound/VSTi, CAW's C++ PD host, the SC NRT renderer,
    // the C++ sample engine), one part per track - no Csound in the PD/SC/sample render paths -
    // and the parts mix down into one file. Frozen tracks join as their rendered WAV.
    {
        g_exportOff = ExportOffline{};
        g_exportOff.active = true;
        g_exportOff.format = format;
        g_exportOff.wavFormat = format >= 3 ? 2 : format;
        g_exportOff.rate = std::max(8000, g_proj.cs.sr);
        double hi = 0.0;
        for (const Track& t : g_proj.tracks) {
            if (t.kind == 1) continue;
            for (const Clip& c : t.clips) hi = std::max(hi, c.start + c.dur);
            for (const MidiClip& mc : t.midiClips) hi = std::max(hi, mc.start + mc.dur);
            for (const auto& lane : t.automation)
                for (const AutoPoint& ap : lane.second) hi = std::max(hi, ap.t);
            // A locked track's sound runs to the end of its rendered WAV: the mix must not stop
            // before the tail does (the offline file used to cut off mid-decay, shorter than the
            // real-time capture of the same project).
            if (trackFrozen(t)) hi = std::max(hi, t.freezeEnd);
        }
        g_exportOff.t0 = std::max(0.0, g_proj.cs.exportStart);
        const double autoEnd = hi + std::max(0.0, g_proj.cs.tail) + 1.0;
        g_exportOff.t1 = g_proj.cs.exportEnd > g_exportOff.t0 ? std::min(g_proj.cs.exportEnd, autoEnd) : autoEnd;
        if (g_exportOff.t1 <= g_exportOff.t0) {
            g_exportError = "The export range is empty (End must be greater than Start).";
            logLine("[caw] " + g_exportError);
            g_exportOff.active = false;
            return;
        }
        logLine("[caw] Offline export range: " + std::to_string(g_exportOff.t0) + " s to " + std::to_string(g_exportOff.t1) + " s.");
        g_exportOff.finalPath = renderFilePath().string();
        g_exportOff.asReal = g_proj.cs.exportSource == 1;
        g_exportOff.splitExport = g_proj.cs.exportSplit;
        g_exportOff.partCh = g_exportOff.asReal ? masterChannels(g_proj.cs) : 2;   // as real routing renders the parts at the whole virtual bus width
        g_renderPath = g_exportOff.finalPath;
        std::error_code ec2;
        g_renderWillOverwrite = std::filesystem::exists(g_exportOff.finalPath, ec2);
        const auto stamp2 = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path dir2 = std::filesystem::path(g_exportOff.finalPath).parent_path();
        const std::string stem2 = ".caw_export_" + std::to_string(stamp2);
        g_exportOff.tempWav = (dir2 / (stem2 + ".wav")).string();
        g_renderTemp = g_exportOff.tempWav;
        g_renderEncodedTemp = format >= 3 ? (dir2 / (stem2 + renderExtension(format))).string() : std::string();
        g_activeRenderFormat = format;
        g_activeFlacBits = g_proj.cs.exportFlacBits;
        g_activeMp3Kbps = g_proj.cs.exportMp3Kbps;
        g_renderConverting = false;
        for (size_t i = 0; i < g_proj.tracks.size(); ++i) {
            const Track& t = g_proj.tracks[i];
            // Solo/mute are part of the mix the user hears, so the export applies the same rule:
            // a muted track never exports, and while any track is soloed only soloed ones do.
            if (t.muted || t.kind == 1 || t.kind == 2) continue;
            bool anySolo = false;
            for (const Track& o : g_proj.tracks) if (o.solo && !o.muted) anySolo = true;
            if (anySolo && !t.solo) continue;
            const bool hasContent = trackFrozen(t) || !t.clips.empty() || !t.midiClips.empty();
            if (!hasContent) continue;
            double tlo = 1e300, thi = 0.0;   // outside the export range: skip the pass entirely
            if (trackFrozen(t)) { tlo = t.freezeStart; thi = t.freezeEnd; }
            else {
                for (const Clip& c : t.clips) { tlo = std::min(tlo, c.start); thi = std::max(thi, c.start + c.dur); }
                for (const MidiClip& mc : t.midiClips) { tlo = std::min(tlo, mc.start); thi = std::max(thi, mc.start + mc.dur); }
            }
            if (thi <= g_exportOff.t0 || tlo >= g_exportOff.t1) continue;
            g_exportOff.pending.push_back((int)i);
        }
        {   // A rack in FX-only mode SILENCES its sources' own sound in the live mix: only the processed
            // signal is heard. The export must do the same - a source (locked or not) that also joined
            // the mix as its own part would sound dry on top of the effect, a sound the live mix never has.
            std::vector<int> drop;
            for (int ri : g_exportOff.pending) {
                const Track& rk = g_proj.tracks[(size_t)ri];
                if (rk.kind != 4 || rk.vstOutMode != 0 || rk.vstPath.empty()) continue;
                for (int si : freezeVsteSources(ri)) drop.push_back(si);
            }
            for (int si : drop) {
                auto it = std::find(g_exportOff.pending.begin(), g_exportOff.pending.end(), si);
                if (it != g_exportOff.pending.end()) {
                    logLine("[caw] Export: \"" + g_proj.tracks[(size_t)si].name + "\" only sounds through its FX-only rack, so it does not export on its own.");
                    g_exportOff.pending.erase(it);
                }
            }
        }
        g_exportOff.startedAt = std::chrono::steady_clock::now();
        g_renderActive = true;
        diagClearAll();
        logLine("[caw] Offline export: " + std::to_string(g_exportOff.pending.size()) +
                " track(s) render one by one through their own engines, then the parts mix into " + g_exportOff.finalPath);
        exportNextTrack();
        return;
    }
    diagClearAll();
    if (g_playing) stopPlayback();
    const std::string csdPath = (workDir() / "caw_render.csd").string();
    g_renderPath = renderFilePath().string();
    g_activeRenderFormat = format;
    g_activeFlacBits = g_proj.cs.exportFlacBits;
    g_activeMp3Kbps = g_proj.cs.exportMp3Kbps;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path dir = std::filesystem::path(g_renderPath).parent_path();
    const std::string stem = ".caw_render_" + std::to_string(stamp);
    g_renderTemp = (dir / (stem + ".wav")).string();
    g_renderEncodedTemp = format >= 3 ? (dir / (stem + renderExtension(format))).string() : std::string();
    g_renderConverting = false;
    { std::error_code rec; std::filesystem::remove(scoreProbePath(), rec); }   // the score probe writes a fresh file
    if (!writeTextFile(csdPath, buildCsd(g_proj, "render", 0.0, &g_orcSpans))) {
        g_exportError = "Could not write the temporary render.";
        logLine("[caw] ERROR: could not write " + csdPath);
        cleanupRenderTemps();
        return;
    }
    if (!ensureCsoundMathProbe()) {
        g_exportError = "This Csound cannot compile abs or sqrt - the generated project cannot compile. Pick a full csound.exe in Settings > Programs (Browse opens CAW's own folder).";
        logLine("[caw] ERROR: " + g_exportError);
        cleanupRenderTemps();
        return;
    }
    std::string error;
    shareEngineDlls();
    if (!g_offlineProc.prepare(csoundExePath(), error)) {
        g_exportError = "Audio engine unavailable: " + error;
        logLine("[caw] " + g_exportError);
        cleanupRenderTemps();
        return;
    }
    vstui::captureAllParameters(g_proj);   // the export renders exactly what the plugin GUIs show
    g_offlineProc.setVstJobs(g_langVst ? collectVstJobs(g_proj, {}) : std::vector<VstJob>{});   // VST unchecked: not loaded in the render either
    g_offlineProc.setControlChannels(vstGateValues());   // the source gates for the render too
    g_offlineProc.setOfflineSampleStreams(collectOfflineSampleStreams(g_proj));   // the locked sources' WAV voices feed the render's VSTe inputs
    const int wavFormat = format >= 3 ? 2 : format;
    if (!g_offlineProc.startOffline(csdPath, g_proj.cs.sr, masterChannels(g_proj.cs),
                                    (float)g_proj.cs.zdbfs, g_proj.cs.masterGain,
                                    wavFormat, g_renderTemp, error, channels)) {   // channels = the exported width: as virtual = the Channels combo, as real routing = the virtual bus, Convert folds to mono/stereo/quadro
        g_exportError = "Audio export failed: " + error;
        logLine("[caw] " + g_exportError);
        cleanupRenderTemps();
        return;
    }
    logLine("[caw] Rendering through CAW's C++ audio and VST host.");
    g_renderActive = true;
}
#include "app.h"
void startRender() {
    if (g_renderActive) return;
    std::error_code ec;
    g_renderWillOverwrite = std::filesystem::exists(renderFilePath(), ec);
    if (ec) { g_exportError = "Could not check output: " + ec.message(); logLine("[caw] " + g_exportError); return; }
    g_renderWarnVst = false;   // the VST parameter-warning popup is gone (user request): exporting starts directly
    if (g_renderWillOverwrite) g_confirmRenderOverwrite = true;
    else startRenderNow();
}
#include "app.h"
void cancelRender() {
    g_offlineProc.stop();
    g_codecProc.stop();
    g_renderProc.stop();
    exportRtCancel();
    if (g_exportOff.active) {
        g_exportOff.pending.clear();
        g_exportOff.active = false;
        logLine("[caw] Offline export cancelled.");
    }
    if (freezeActive()) freezeCancel();   // a per-track part pass may be mid-flight
    for (const std::string& l : g_offlineProc.takeOutput()) logLine(l);
    for (const std::string& l : g_renderProc.takeOutput()) logLine(l);
    g_renderActive = false;
    cleanupRenderTemps();
    logLine("[caw] Render cancelled.");
}
#include "app.h"
bool startFallbackEncoder() {
    std::error_code ec;
    std::filesystem::remove(g_renderEncodedTemp, ec);
    std::string ffmpeg = plat::bundledFfmpeg();
    if (ffmpeg.empty() && !g_proj.cs.ffmpegExe.empty()) {
        std::error_code pathError;
        if (std::filesystem::is_regular_file(g_proj.cs.ffmpegExe, pathError))
            ffmpeg = g_proj.cs.ffmpegExe;
        else
            logLine("[caw] Configured FFmpeg was not found: " + g_proj.cs.ffmpegExe);
    }
    if (ffmpeg.empty()) ffmpeg = plat::findOnPath("ffmpeg");
    if (ffmpeg.empty()) return false;
    const std::string codec = g_activeRenderFormat == 3
        ? " -codec:a flac -sample_fmt " + std::string(g_activeFlacBits == 16 ? "s16" : "s32") +
          " -bits_per_raw_sample " + std::to_string(g_activeFlacBits) + " "
        : " -codec:a libmp3lame -b:a " + std::to_string(g_activeMp3Kbps) + "k ";
    const std::string cmd = processArg(ffmpeg) + " -nostdin -n -i " + processArg(g_renderTemp)
        + codec + processArg(g_renderEncodedTemp);
    return g_renderProc.start(cmd);
}
#include "app.h"
void tickAutosave(float dt) {
    if (freezeActive()) { g_autoSaveElapsed = 0.f; return; }   // a freeze render mutates mute / solo temporarily: autosave skips it
    if (!g_proj.cs.autosaveEnabled) { g_autoSaveElapsed = 0.f; return; }
    g_autoSaveElapsed += std::max(0.f, dt);
    const int minutes = std::min(120, std::max(1, g_proj.cs.autosaveMinutes));
    if (g_autoSaveElapsed < minutes * 60.f || historyBusy()) return;
    g_autoSaveElapsed = 0.f;
    vstui::captureParameters(g_proj);
    const std::string snapshot = serializeProject(g_proj);
    if (snapshot == g_savedSnap || snapshot == g_autoSavedSnap) return;
    std::filesystem::path target;
    if (!g_projectPath.empty()) {
        target = std::filesystem::path(g_projectPath);
        target += ".autosave.caw";
    } else {
        if (g_unsavedAutosavePath.empty()) {
            std::string home = plat::getEnv(plat::isWindows ? "USERPROFILE" : "HOME");
            if (home.empty()) home = workDir().string();
            const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
            g_unsavedAutosavePath = (std::filesystem::path(home) / "CAW Autosaves" /
                ("Untitled-" + std::to_string(stamp) + ".autosave.caw")).string();
        }
        target = g_unsavedAutosavePath;
    }
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) { logLine("[caw] Autosave folder failed: " + ec.message()); return; }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string tmp = target.string() + "." + std::to_string(stamp) + ".tmp";
    std::string error;
    if (!saveProjectFile(g_proj, tmp, error)) {
        logLine("[caw] Autosave failed: " + error);
        std::filesystem::remove(tmp, ec);
        return;
    }
    if (!replaceOutputFile(tmp, target.string(), error)) {
        logLine("[caw] Autosave failed: " + error);
        std::filesystem::remove(tmp, ec);
        return;
    }
    g_autoSavedSnap = snapshot;
    logLine("[caw] Recovery autosave: " + target.string());
}
#include "app.h"
double engineOutputLatency() {
    const double sync = (double)g_proj.cs.syncMs / 1000.0;   // Settings > Audio > Sound sync: tuned by ear / eye until the sound meets the playhead
    return std::max(0.0, std::min(1.0, (double)std::max(0, g_proj.cs.bufHW) / (double)std::max(1000, g_proj.cs.sr) + sync));
}
#include "app.h"
cawaudio::AudioHandle audioClipData(const Clip& c, std::string& error) {
    return cawaudio::getAudio(c.audioFile, g_proj.cs.sr, g_proj.cs.ffmpegExe, error);
}
#include "app.h"
double audioSegsTotal(const Clip& c) { double t = 0.0; for (const AudioSeg& s : c.audioSegs) t += s.dur; return t; }
#include "app.h"
std::array<int, 8> audioClipOutMap(const Clip& c, const cawaudio::AudioHandle& data) {
    std::array<int, 8> map = {{0, -1, -1, -1, -1, -1, -1, -1}};
    const int srcCh = data ? std::max(1, data->channels) : 2;
    // "as the source": input channel k plays output k (one output per file channel). Custom
    // routing lives in the clip's CAW code (out1 = in1 ...) and overrides this live per block.
    for (int k = 0; k < std::min(8, srcCh); ++k) map[(size_t)k] = k;
    return map;
}
#include "app.h"
unsigned long long audioClipSignature(const Clip& c, float trackVolume )
{   // a voice is rebuilt when this changes
    unsigned long long h = 1469598103934665603ULL;
    auto mix = [&](unsigned long long v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
    mix((unsigned long long)std::hash<std::string>{}(c.audioFile));
    // NOTE: the CAW code is NOT part of the signature - live-coding: edits apply only on Run
    // (Run recompiles the script, resizes the clip and restarts the voice).
    mix((unsigned long long)(c.start * 1000.0));
    mix((unsigned long long)(c.dur * 1000.0));
    mix((unsigned long long)(c.audioRate * 1000.0));
    mix(c.audioReverse ? 7u : 3u);
    mix((unsigned long long)(c.audioGain * 1000.0));
    mix((unsigned long long)(trackVolume * 1000.0));   // the track volume slider (a change rebuilds the voices)
    mix((unsigned long long)c.audioGainPts.size());
    for (const AutoPoint& p : c.audioGainPts) { mix((unsigned long long)(p.t * 1000.0)); mix((unsigned long long)(p.v * 1000.0)); }
    for (const AudioSeg& s : c.audioSegs) { mix((unsigned long long)(s.srcOffset * 1000.0)); mix((unsigned long long)(s.dur * 1000.0)); }
    return h;
}
#include "app.h"
cawaudio::VoiceSpec audioClipVoiceSpec(const Clip& c, int trackId, double from, float trackVolume )
{
    cawaudio::VoiceSpec spec;
    spec.kind = 0;
    spec.trackId = trackId;
    spec.clipId = c.id;
    spec.clipStart = c.start;
    spec.clipDur = c.dur;
    spec.segs = c.audioSegs;
    // the code's rate is the clip's LENGTH scale (rate = 2 -> twice as long, half speed):
    // the engine's base speed is its inverse
    spec.rate = 1.0 / std::max(0.05, (double)c.audioRate);
    spec.reverse = c.audioReverse;
    {
        double rC = 1.0, gC = 1.0; bool revC = false, normC = false;
        cawaudio::sampleCodeVisuals(c.code, rC, gC, revC, normC);
        spec.normalize = normC;   // the code's normalize = 1: the engine scales the file to full level
    }
    spec.gain0 = c.audioGain * (double)std::max(0.f, std::min(kMaxVolumeGain, trackVolume));   // the track volume slider
    for (const AutoPoint& p : c.audioGainPts) spec.gainPts.push_back({p.t, (float)p.v});
    spec.t0 = std::max(from, c.start) + engineOutputLatency();   // the sound meets the playhead (skip the latency into the clip)
    spec.skip = std::max(0.0, std::max(from, c.start) - c.start) * spec.rate;   // source seconds the playhead is already into the clip
    spec.delay = 0;
    std::string err;
    spec.data = audioClipData(c, err);
    spec.script = sampleScript(c);
    spec.outSrc = audioClipOutMap(c, spec.data);
    return spec;
}
#include "app.h"
void audioKillClipVoices(int clipId) { cawaudio::engine().killClip(clipId); g_audioVoiceSig.erase(clipId); }
#include "app.h"
void masterChainRender(float* buffer, unsigned long frames, int channels) noexcept {
    if (!buffer || frames == 0 || channels <= 0) return;
    // Master FX is a VST feature: it only processes while the VST engine runs (CAW: VST checked + DSP on).
    // Unchecked or not started: the master mix passes through untouched.
    if (g_vstEngineOn) masterfx::rack().render(buffer, (int)frames, channels);   // try-locks internally: skips the block if the UI holds the rack
    const float gain = g_proj.cs.masterGain;
    if (gain != 1.f) {
        const size_t n = (size_t)frames * (size_t)channels;
        for (size_t i = 0; i < n; ++i) buffer[i] *= gain;
    }
}
#include "app.h"
void closeSampleOutput() {
    if (g_sampleOutput) { g_sampleOutput->close(); g_sampleOutput.reset(); }
    g_sampleCapture.reset();
    g_sampleSnapshotSeq = 0;
}
#include "app.h"
void ensureSampleOutput() {
    if (!g_langAudio || !g_sampleEngineOn || g_sampleOutput) return;
    if (scHasSharedAudioOwner() || g_scSoloOutput || g_vstSoloOutput || g_testSound.running() || g_renderActive) return;   // another stream already calls the sample render
    if (!g_audioDevicesReady) refreshAudioDevices(false);
    if (g_proj.cs.outDev == -2) return;   // no output chosen: Settings already asks for one
    CawAudioSettings settings = pdAudioSettings();
    // The shared hook (scRealtimeSource, attached by pdAudioSettings): the sample stream also mixes
    // the VST rack into the device - whichever CAW stream is open carries everything.
    std::string error;
    auto output = std::make_unique<CawAudioOutput>();
    auto capture = std::make_unique<MonitorCapture>(settings.sampleRate, settings.channels);
    output->setMonitorCapture(capture.get());
    if (output->open(settings, error)) {
        output->setExpectingAudio(false);
        g_sampleCapture = std::move(capture);
        g_sampleOutput = std::move(output);
        if (g_sampleOutput->actualRate() != settings.sampleRate)
            logLine("[caw] WARNING: the output device runs at " + std::to_string(g_sampleOutput->actualRate()) + " Hz but the project is " + std::to_string(settings.sampleRate) + " Hz - sounds play at the wrong speed/pitch. Set the project sample rate to the device rate in Settings > Audio.");
        logLine("[caw] Sample engine: playing through " + g_sampleOutput->description());
    } else {
        logLine("[caw] Sample output: " + error + " - the samples stay silent until an output opens.");
    }
}
#include "app.h"
void applySampleClip(Clip& c, int trackId) {
    g_sampleScripts.erase(c.id);   // recompile the code
    g_audioVoiceSig.erase(c.id);
    cawaudio::engine().killClip(c.id);
    std::string err;
    const cawaudio::AudioHandle data = audioClipData(c, err);
    if (!data || data->samples.empty()) { logLine("[caw] Sample clip: no audio: " + err); g_clipCodeDirty.erase(c.id); return; }
    // The code appears in the clip: its plain constants become the clip's look and length
    // (Run applies them, like every other language - the timeline never changes while typing).
    //   rate = 0.5 -> the clip is half as long (double speed), rate = 2 -> twice as long
    //   gain = 2   -> the waveshape / spectrogram is drawn with twice the amplitude
    //   normalize = 1 -> the file is scaled to full level before the gain
    //   reverse = 1 -> the shape is mirrored and the clip plays backwards
    double rCode = 1.0, gCode = 1.0; bool revCode = false, normCode = false;
    cawaudio::sampleCodeVisuals(c.code, rCode, gCode, revCode, normCode);
    rCode = std::min(16.0, std::max(0.05, rCode));
    if (std::fabs(rCode - (double)c.audioRate) > 1e-9) {   // resize the clip so the sound fills the box
        const double segsTotal = audioSegsTotal(c);
        c.dur = std::max(0.001, std::min(c.dur * rCode / std::max(0.05, (double)c.audioRate), segsTotal * rCode));
        c.audioRate = (float)rCode;
    }
    c.audioGain = (float)std::max(0.0, gCode);
    if (normCode && data->peak() > 1e-6f) c.audioGain = (float)std::max(0.0, gCode / (double)data->peak());   // full level before the gain (the shape shows it too)
    c.audioReverse = revCode;
    const bool inWindow = g_timelineRun && c.start <= g_play + 1e-9 && g_play < c.start + c.dur;
    if (inWindow) {
        cawaudio::engine().setClipVoice(audioClipVoiceSpec(c, trackId, g_play, trackVolumeOf(trackId)));
        g_audioVoiceSig[c.id] = audioClipSignature(c, trackVolumeOf(trackId));
    } else if (!g_sampleEngineOn) {
        // DSP OFF: Run plays the clip once for its duration - the sample engine is started here
        // (the same way the freeze dialog turns it on). DSP ON: Run is an APPLY button only - no
        // audition with the transport stopped; the playing window above restarts with the new code.
        g_sampleEngineOn = true;
        ensureSampleOutput();
        logLine("[caw] The sample engine was off - it is running now; the clip is auditioned once.");
        cawaudio::VoiceSpec spec;
        spec.kind = 2;   // manual audition: not managed by the timeline windows
        spec.trackId = trackId;
        spec.clipId = c.id;
        spec.clipStart = g_play;
        spec.clipDur = c.dur;
        spec.segs = c.audioSegs;
        spec.rate = 1.0 / std::max(0.05, (double)c.audioRate);   // the code's rate is the clip's length scale: speed = 1/rate
        spec.reverse = c.audioReverse;
        {
            double rC = 1.0, gC = 1.0; bool revC = false, normC = false;
            cawaudio::sampleCodeVisuals(c.code, rC, gC, revC, normC);
            spec.normalize = normC;
        }
        spec.gain0 = c.audioGain * (double)trackVolumeOf(trackId);   // the track volume slider
        for (const AutoPoint& p : c.audioGainPts) spec.gainPts.push_back({p.t, (float)p.v});
        spec.t0 = g_play + engineOutputLatency();
        spec.delay = 0;
        spec.data = data;
        spec.outSrc = audioClipOutMap(c, spec.data);
        spec.script = sampleScript(c);
        cawaudio::engine().setClipVoice(spec);
    }
    g_clipCodeDirty.erase(c.id);
    logLine("[caw] Sample clip applied: " + c.name);
}
#include "app.h"
// The rate the soundcard REALLY runs at (a device may ignore the requested project rate): the
// sample engine's voices must advance at the device's pace no matter which CAW stream carries
// them - the SuperCollider engine's pump and the sample render live in the same callback.
int activeStreamRate(int fallback) {
    if (g_scSoloOutput && g_scSoloOutput->actualRate() > 0) return g_scSoloOutput->actualRate();
    if (g_sampleOutput && g_sampleOutput->actualRate() > 0) return g_sampleOutput->actualRate();
    if (g_vstSoloOutput && g_vstSoloOutput->actualRate() > 0) return g_vstSoloOutput->actualRate();
    return fallback;
}
void scheduleAudioTimeline(double from, bool restart) {
    cawaudio::Engine& eng = cawaudio::engine();
    eng.setSr(activeStreamRate(g_proj.cs.sr));
    if (!g_timelineRun) { eng.clearAll(); g_audioVoiceSig.clear(); g_audioLastFrom = from; return; }
    // Frozen (LKD) tracks play their rendered WAV through the sample engine EVEN when the sample
    // DSP is off: the WAV is just a stream, not a sample clip - a project without any sample track
    // never turns the sample engine on, and the frozen WAV still has to play.
    bool anyFrozen = false;
    for (const Track& t : g_proj.tracks)
        if (trackFrozen(t) && !trackHiddenByFolder(t)) { anyFrozen = true; break; }
    if (!g_sampleEngineOn && !anyFrozen) { eng.clearAll(); g_audioVoiceSig.clear(); g_audioLastFrom = from; return; }
    if (restart) { eng.clearAll(); g_audioVoiceSig.clear(); g_audioLastFrom = from; }
    bool anySolo = false;
    for (const Track& t : g_proj.tracks) if (t.solo && !t.muted) anySolo = true;
    std::set<int> want;
    const bool sampleDsp = g_sampleEngineOn;
    for (size_t ti = 0; ti < g_proj.tracks.size() && sampleDsp; ++ti) {
        const Track& t = g_proj.tracks[ti];
        if (t.kind != 7 || trackHiddenByFolder(t)) continue;
        const bool audible = !t.muted && !(anySolo && !t.solo) && !trackFrozen(t);   // frozen kind 7: the frozen WAV plays instead (loop below)
        if (!trackFrozen(t)) eng.setTrackGain(t.id, trackVolumeAutomation(t, from));   // the volume lane is live (0 = silent); frozen: it is baked into the WAV
        for (const Clip& c : t.clips) {
            if (c.muted || c.audioFile.empty() || c.audioSegs.empty()) continue;
            const bool inWindow = c.start <= from + 1e-9 && from < c.start + c.dur;
            if (!audible || !inWindow) continue;
            if (c.midiEnabled) continue;   // sampler mode: the file is NOT played by the timeline - only its MIDI notes start it
            want.insert(c.id);
            const unsigned long long sig = audioClipSignature(c, t.volume);
            if (g_audioVoiceSig.count(c.id) == 0 || g_audioVoiceSig[c.id] != sig) {
                eng.setClipVoice(audioClipVoiceSpec(c, t.id, from, t.volume));
                g_audioVoiceSig[c.id] = sig;
            }
        }
    }
    // frozen tracks: ONE virtual clip per frozen track plays its rendered WAV (the live engines
    // skip the track everywhere else). The track volume slider is carried live (the signature
    // includes it, a fader move rebuilds the voice); the "volume" automation lane is already
    // baked into the WAV, so setTrackGain is deliberately NOT called here.
    for (const Track& t : g_proj.tracks) {
        if (!trackFrozen(t) || trackHiddenByFolder(t)) continue;
        const bool audible = !t.muted && !(anySolo && !t.solo);
        const int vid = freezeClipId(t.id);
        const bool inWindow = t.freezeStart <= from + 1e-9 && from < t.freezeEnd;
        if (!audible || !inWindow) continue;
        want.insert(vid);
        Clip fc;                                      // a throwaway clip shape: only the audio fields matter
        fc.id = vid;
        fc.start = t.freezeStart;
        fc.dur = std::max(0.01, t.freezeEnd - t.freezeStart);
        fc.audioFile = t.freezePath;
        fc.audioSegs = {{0.0, fc.dur}};
        fc.audioRate = 1.f;
        fc.audioGain = 1.f;
        const unsigned long long sig = audioClipSignature(fc, t.volume);
        if (g_audioVoiceSig.count(vid) == 0 || g_audioVoiceSig[vid] != sig) {
            const cawaudio::VoiceSpec fs = audioClipVoiceSpec(fc, t.id, from, t.volume);
            eng.setClipVoice(fs);
            g_audioVoiceSig[vid] = sig;
            const cawaudio::AudioHandle fd = fs.data;
            char b[3][32];
            std::snprintf(b[0], sizeof b[0], "%.3f", fs.t0);
            std::snprintf(b[1], sizeof b[1], "%.3f", fs.skip);
            std::snprintf(b[2], sizeof b[2], "%.3f", (double)g_proj.cs.sr);
            logLine("[caw] Freeze play '" + t.name + "': voice built t0=" + b[0] + " skip=" + b[1] +
                    " dur=" + std::to_string(fc.dur) + " fileCh=" + std::to_string(fd ? fd->channels : -1) +
                    " fileSr=" + (fd ? std::to_string(fd->sr) : "?") + " engSr=" + b[2]);
        }
    }
    for (auto it = g_audioVoiceSig.begin(); it != g_audioVoiceSig.end();)
        if (want.count(it->first)) ++it; else { eng.killManaged(it->first); it = g_audioVoiceSig.erase(it); }

    // sampler notes: the notes of the MIDI lane trigger the clip like a sample
    const double lookahead = engineOutputLatency() + 0.06;
    for (size_t ti = 0; ti < g_proj.tracks.size() && sampleDsp; ++ti) {
        const Track& t = g_proj.tracks[ti];
        if (t.kind != 7 || trackHiddenByFolder(t) || t.midiClips.empty()) continue;
        const bool audible = !t.muted && !(anySolo && !t.solo) && !trackFrozen(t);
        if (!audible) continue;
        for (const MidiClip& mc : t.midiClips) {
            if (mc.muted) continue;
            const Clip* target = findClip(mc.clipId);
            if (!target || target->muted || target->audioFile.empty() || target->audioSegs.empty()) continue;
            std::string err;
            const cawaudio::AudioHandle data = audioClipData(*target, err);
            if (!data || data->samples.empty()) continue;
            const double totalSrc = data->duration();
            const double srcStart = target->audioSegs.front().srcOffset;
            double nR = 1.0, nG = 1.0; bool nRev = false, nNorm = false;
            cawaudio::sampleCodeVisuals(target->code, nR, nG, nRev, nNorm);
            for (const MidiNote& note : mc.notes) {
                const double onset = mc.start + note.start;
                if (!(g_audioLastFrom < onset && onset <= from + lookahead)) continue;
                cawaudio::VoiceSpec spec;
                spec.kind = 1;
                spec.trackId = t.id;
                spec.clipId = target->id;
                spec.t0 = onset + engineOutputLatency();
                spec.delay = (long long)std::llround(std::max(0.0, spec.t0 - from) * (double)std::max(8000, g_proj.cs.sr));
                const double pitchRatio = std::pow(2.0, (double)(note.pitch - 60) / 12.0);
                spec.rate = (1.0 / std::max(0.05, (double)target->audioRate)) * pitchRatio;   // the code's rate is the length scale: speed = 1/rate
                spec.reverse = target->audioReverse;
                spec.normalize = nNorm;   // the code's normalize = 1 scales the file to full level first
                spec.gain0 = target->audioGain * (double)note.velocity / 100.0 * (double)trackVolumeOf(t.id);
                // The note plays the file from where the TIMELINE points: the note's onset inside
                // the clip window maps through the segments (like the shape view) - a note at the
                // 2-second point of the clip starts the file at its 2-second position, not at zero.
                double srcPos = srcStart;
                {
                    double lt = onset - target->start;   // clip-local seconds at the note's onset
                    if (lt < 0.0) lt = 0.0;
                    const double speed = 1.0 / std::max(0.05, (double)target->audioRate);   // source seconds per clip second
                    const double want = lt * speed;
                    if (target->audioReverse) {
                        double rem = want;
                        srcPos = srcStart;
                        for (auto it = target->audioSegs.rbegin(); it != target->audioSegs.rend(); ++it) {
                            if (rem < it->dur) { srcPos = it->srcOffset + it->dur - rem; break; }
                            rem -= it->dur;
                            if (std::next(it) == target->audioSegs.rend()) srcPos = it->srcOffset;   // past the chain: the very start
                        }
                    } else {
                        double rem = want;
                        srcPos = srcStart;
                        for (const AudioSeg& s : target->audioSegs) {
                            if (rem < s.dur) { srcPos = s.srcOffset + rem; break; }
                            rem -= s.dur;
                            if (&s == &target->audioSegs.back()) srcPos = s.srcOffset + s.dur;   // past the chain: the very end
                        }
                    }
                }
                spec.srcOffset = srcPos;
                const double avail = spec.reverse ? srcPos : std::max(0.0, totalSrc - srcPos);
                // The MIDI note GATES the sound: the sample plays while the note is held and stops at
                // note-off (a shorter file just ends earlier). One-shot = "available" only limited how
                // long the file could go; now the note length is what stops it.
                spec.noteDur = std::max(0.02, std::min((double)note.dur, std::max(0.02, avail / spec.rate)));
                if (spec.noteDur <= 0.02) continue;
                spec.data = data;
                spec.outSrc = audioClipOutMap(*target, data);
                spec.script = sampleScript(*target);
                eng.startVoice(spec);
            }
        }
    }
    g_audioLastFrom = from;
}
#include "app.h"
bool audioTracksExist() {
    for (const Track& t : g_proj.tracks)
        if (t.kind == 7 && !trackHiddenByFolder(t))
            for (const Clip& c : t.clips)
                if (!c.muted && !c.audioFile.empty() && !c.audioSegs.empty()) return true;
    return false;
}
#include "app.h"
void audioClockSync(double playhead) { g_audioClockAnchor = cawaudio::engine().timeSeconds() - playhead; }
#include "app.h"
void audioTrimLeft(Clip& c, double delta) {
    if (c.audioSegs.empty() || delta == 0.0) return;
    const double rate = std::max(0.05, (double)c.audioRate);   // the code's rate is the length scale: source consumed = timeline / rate
    if (delta > 0.0) {   // the left edge moved right: consume source from the front
        double need = delta / rate, total = 0.0;
        for (const AudioSeg& s : c.audioSegs) total += s.dur;
        need = std::min(need, total);
        std::vector<AudioSeg> segs;
        double acc = 0.0;
        for (const AudioSeg& s : c.audioSegs) {
            if (acc >= need) { segs.push_back(s); continue; }
            const double take = std::min(s.dur, need - acc);
            if (s.dur - take > 1e-9) segs.push_back({s.srcOffset + take, s.dur - take});
            acc += take;
        }
        if (!segs.empty()) c.audioSegs = segs;
        for (AutoPoint& p : c.audioGainPts) p.t = std::max(0.0, p.t - delta);
    } else {   // the left edge moved left: extend the first segment backwards when the source allows it
        const double ext = -delta / rate;
        AudioSeg& first = c.audioSegs.front();
        const double room = std::max(0.0, first.srcOffset);
        const double take = std::min(ext, room);
        if (take > 1e-9) {
            first.srcOffset -= take;
            first.dur += take;
        }
        for (AutoPoint& p : c.audioGainPts) p.t = p.t - delta;
    }
}
#include "app.h"
void mixedPdTransport(bool playing) {
    if(g_mixedPd)
        g_apiProc.pdTransport(playing,g_play,g_proj.loopStart,g_proj.loopEnd,timeline_loop::valid(g_proj),g_proj.cs.sr,g_cmdSerial);
}
#include "app.h"
#include "track_gain.h"
// The track fader x volume lane (the same rule as the sample engine) pushed to the engines whose audio CAW
// does not mix as samples: live Csound (cawvol_<trackId> channels), PD (Sink::tick) and SuperCollider (per
// slot). Frozen tracks play their WAV through the sample engine, so they are left at unity here.
void pushEngineTrackGains(double time, bool force) {
    std::vector<std::pair<std::string, double>> csVol;
    for (const Track& t : g_proj.tracks) {
        if (t.kind != 0 && t.kind != 5 && t.kind != 6) continue;
        const float g = trackFrozen(t) ? 1.f : std::max(0.f, std::min(kMaxVolumeGain, t.volume)) * trackVolumeAutomation(t, time);
        if (t.kind == 0) csVol.push_back({"cawvol_" + std::to_string(t.id), (double)g});
        else if (t.kind == 6) trackGains().set(t.id, g);
        else { const int slot = scTrackSlot(t.id); if (slot >= 0) g_scEngine.setSlotGain(slot, g); }
    }
    static std::vector<std::pair<std::string, double>> lastCs;
    if (force) lastCs.clear();   // a (re)start: the engine may be a new Csound instance
    if (g_liveCsound && g_useCsoundApi && csVol != lastCs) { g_apiProc.setControlChannels(csVol); lastCs = csVol; }
    if (!g_liveCsound) lastCs.clear();
}
void tickTimeline(float dt) {
    if (g_drag == Drag::Playhead) { g_resync = 1; return; }  // wait until the playhead is released
    if (timeline_loop::valid(g_proj) && (g_play < g_proj.loopStart || g_play >= g_proj.loopEnd)) {
        g_play = timeline_loop::wrap(g_proj, g_play);
        g_resync = 1;
    }
    const bool scUp = g_scReady && g_scEngine.running();
    // The 4th engine (the VSTi rack) owns the playhead clock when nothing else does; with only
    // sample tracks playing, the sample engine is the clock instead (audio-rate accurate).
    // Its clock is only alive when a callback actually runs (own stream, or a shared one).
    const bool sampleCallbackAlive = g_sampleEngineOn && (g_sampleOutput != nullptr || g_vstSoloOutput != nullptr || g_scSoloOutput != nullptr || scHasSharedAudioOwner());
    const bool audioOnly = audioTracksExist() && sampleCallbackAlive && !engineIsUp() && !scUp && !g_vstEngineOn && !g_pdTimelineMode;
    vstSolo().setDrivesTransport(!engineIsUp() && !scUp && !audioOnly);
    // The VST engine counts as UP while it RUNS - plugins or not. Without this, a session with
    // VST checked but no plugin loaded took the silent-playhead path every tick and never reached
    // scheduleAudioTimeline: the sample voices (and every other engine's clips) were never
    // scheduled - no sound, no meters - exactly the "engines do not work together" bug.
    const bool vstUp = g_vstEngineOn;
    const bool audioUp = engineIsUp() || scUp || vstUp || audioOnly;
    if (!audioUp) {
        g_play = timeline_loop::wrap(g_proj, g_play + dt);  // silent playhead move
        g_resync = 1;
        return;
    }
    if (g_resync) {              // started, or the playhead jumped: silence, then play from here
        g_resync = 0;
        vstEngineJoinTransport();   // the VST engine: fresh schedule from the MIDI clips, play from the playhead
        if (engineIsUp()) commandPlayFrom(g_play, true);
        if (scUp) scheduleScTimeline(g_play, true);
        scheduleAudioTimeline(g_play, true);
        if (audioOnly) audioClockSync(g_play);
        mixedPdTransport(true);
        pushEngineTrackGains(g_play, true);
        g_sig = timelineSignature(g_proj);
        g_scTimelineSig = scTimelineSignature(g_proj);
        g_sigT = 0.0; g_scPlanT = 0.0;
        g_hold = engineOutputLatency();
        return;
    }
    if (g_hold > 0.0) { g_hold -= dt; return; }   // the playhead meets the sound (one sound-card buffer)
    // The soundcard is the clock when the 4th engine drives the transport (audio-rate accurate);
    // otherwise the frame clock advances the playhead as before.
    if (vstSolo().drivesTransport()) {
        g_play = vstSolo().timeSeconds();
        // A plugin load / window open can stall the engine clock: if it escaped the loop region by a
        // wide margin, re-anchor instead of leaving the loop (samples loop through THIS playhead).
        if (timeline_loop::valid(g_proj) && (g_play < g_proj.loopStart - 1.0 || g_play > g_proj.loopEnd + 1.0)) {
            g_play = timeline_loop::wrap(g_proj, g_play);
            g_resync = 1;
        }
    }
    else if (audioOnly) g_play = cawaudio::engine().timeSeconds() - g_audioClockAnchor;
    else {
        g_play += dt;
        if (g_vstEngineOn) vstSolo().followClock(g_play);   // stay in time with the Csound / SC transport
    }
    scheduleAudioTimeline(g_play, false);   // sample voices follow the windows (and trigger sampler notes)
    pushEngineTrackGains(g_play);   // the fader x volume lane of live Csound / PD / SuperCollider tracks
    ensureSampleOutput();   // the sample engine keeps an output stream open whenever nothing else owns the card
    pollScAutomation();   // SuperCollider lanes follow the playhead (~cawget in the language)
    {   // play diagnostics (once a second while playing): engine states + who is actually loud
        static double s_diagT = 0.0;
        s_diagT += dt;
        if (g_timelineRun && s_diagT >= 1.0) {
            s_diagT = 0.0;
            int devRate = 0;
            if (g_scSoloOutput) devRate = g_scSoloOutput->actualRate();
            else if (g_sampleOutput) devRate = g_sampleOutput->actualRate();
            else if (g_vstSoloOutput) devRate = g_vstSoloOutput->actualRate();
            char b[2][32];
            std::snprintf(b[0], sizeof b[0], "%.4f", g_diagScRms.load(std::memory_order_relaxed));
            std::snprintf(b[1], sizeof b[1], "%.4f", g_diagCawRms.load(std::memory_order_relaxed));
            int frozenVoices = 0;
            for (const auto& kv : g_audioVoiceSig) if (kv.first <= -1000000) ++frozenVoices;
            logLine("[caw] Play diag: sc=" + std::to_string(g_scEngineOn) + "/" + std::to_string(g_scReady) +
                    " sample=" + std::to_string(g_sampleEngineOn) + " vst=" + std::to_string(g_vstEngineOn) +
                    " csound=" + std::to_string(engineIsUp() ? 1 : 0) + " frozenVoices=" + std::to_string(frozenVoices) +
                    " devRate=" + std::to_string(devRate) + " projSr=" + std::to_string(g_proj.cs.sr) +
                    " scMixRms=" + b[0] + " cawMixRms=" + b[1]);
        }
    }
    if (timeline_loop::valid(g_proj) && g_play >= g_proj.loopEnd) {
        g_play = timeline_loop::wrap(g_proj, g_play);
        // This host reissues transport commands at the boundary. It is not a
        // sample-accurate or gapless loop, but nothing beyond the end is queued.
        if (engineIsUp()) commandPlayFrom(g_play, true);
        if (scUp) scheduleScTimeline(g_play, true);
        scheduleAudioTimeline(g_play, true);
        if (audioOnly) audioClockSync(g_play);
        mixedPdTransport(true);
        pushEngineTrackGains(g_play, true);
        g_sig = timelineSignature(g_proj);
        g_scTimelineSig = scTimelineSignature(g_proj);
        g_sigT = 0.0; g_scPlanT = 0.0;
        return;
    }
    // Re-plan edits occasionally. SC code edits restart its groups from the current playhead.
    g_sigT += dt;
    if (g_sigT >= 0.25) {
        g_sigT = 0.0;
        const unsigned long long sig = timelineSignature(g_proj);
        if (sig != g_sig) {
            g_sig = sig;
            if (engineIsUp()) commandPlayFrom(g_play, false);
            const std::vector<VstSoloNote> notes = buildVstSoloNotes();   // MIDI clips / mute / solo edited while playing
            if (g_vstEngineOn) vstSolo().setNotes(notes);
        }
    }
    if (scUp) {
        g_scPlanT += dt;
        if (g_scPlanT >= 0.25) {
            g_scPlanT = 0.0;
            const unsigned long long sig = scTimelineSignature(g_proj);
            if (sig != g_scTimelineSig) scheduleScTimeline(g_play);
            else scTimelineWindows(g_play);   // playhead entered a new clip window (or a live code edit settled)
        }
    }
}
#include "app.h"
bool makePdPlan(std::vector<pd_timeline::Event>& events, std::string& error, bool includeMuted )
{
    int bad = 0;
    const bool ok = pd_timeline::makePlan(g_proj, g_projectPath, events, error, g_playing || engineIsUp(), &bad, includeMuted);
    if (!ok && bad > 0)   // open the clip that needs attention
        for (Track& t : g_proj.tracks) for (Clip& c : t.clips) if (c.id == bad) c.editorOpen = true;
    return ok;
}
#include "app.h"
void refreshMixedPdPlan(float dt) {
    if(!g_mixedPd || !g_apiProc.pdReady())return;
    for(const auto& note:g_apiProc.takePdPlanNotes())logPdNote(note);
    const auto updateError=g_apiProc.takePdPlanError();
    if(!updateError.empty()) {
        g_pdPlanUpdateFailed=true;g_pdNeedsReload=true;
        playNotice("PD timing update failed: "+updateError,"PD timing update failed: see log");
    }
    if(g_pdPlanUpdateFailed)return;
    g_pdPlanT+=dt;
    if(g_pdPlanT<.25)return;
    g_pdPlanT=0;
    std::vector<pd_timeline::Event> next;std::string error;int bad=0;
    // Same plan shape as the engine holds (muted clips flagged), so M / S is a live replan.
    const bool valid=pd_timeline::makePlan(g_proj,g_projectPath,next,error,false,&bad,true);
    if(valid && !pd_timeline::samePlan(next,g_pdTimelineEvents) &&
       pd_timeline::sameInstances(next,g_pdTimelineEvents) && g_apiProc.replanPd(next)) {
        g_pdTimelineEvents=std::move(next);g_pdNeedsReload=false;return;
    }
    if(!valid || !pd_timeline::samePlan(next,g_pdTimelineEvents)) {
        if(!g_pdNeedsReload) {
            if(g_timelineRun){g_timelineRun=false;commandStopAll();mixedPdTransport(false);stopAllScClips();}
            playNotice(valid ? "PD clips changed. Restart the shared engines before playing." :
                "PD clip problem: "+error,"PD changed: restart engines");
        }
        g_pdNeedsReload=true;
    } else g_pdNeedsReload=false;
}
#include "app.h"
void pdTransport(bool play) {
    g_pdTimelineEngine.transport(play,g_play,g_proj.loopStart,g_proj.loopEnd,timeline_loop::valid(g_proj),g_proj.cs.sr);
}
#include "app.h"
void refreshPdPlan(float dt) {
    if (!g_langPd || !g_pdEngineOn || !g_pdTimelineEngine.ready()) return;
    for(const auto& note:g_pdTimelineEngine.takeNotes())logPdNote(note);
    const auto updateError=g_pdTimelineEngine.takePlanError();
    if(!updateError.empty()) {
        g_pdPlanUpdateFailed=true;g_pdNeedsReload=true;
        playNotice("PD timing update failed: "+updateError,"PD timing update failed: see log");
    }
    if(g_pdPlanUpdateFailed)return;
    g_pdPlanT += dt;
    if (g_pdPlanT < 0.25) return;
    g_pdPlanT = 0.0;
    std::vector<pd_timeline::Event> next; std::string error;
    if (!makePdPlan(next,error,true)) {
        // A PD clip cannot be planned: PD goes quiet, nothing else does. The transport keeps
        // running on the VST / SC clock and the other engines are not touched.
        if (g_pdTimelineMode) { pdTransport(false); g_pdTimelineMode=false; g_pdPlayingClip=0; g_pdResumeAfterReload=false; g_resync=1; }
        if(error!=g_pdLastPlanError){g_pdLastPlanError=error;playNotice("PD clip problem: "+error,"PD clip problem: see the log");}
        g_pdNeedsReload=true;
        return;
    }
    g_pdLastPlanError.clear();
    if (!pd_timeline::samePlan(next,g_pdTimelineEvents)) {
        if(pd_timeline::sameInstances(next,g_pdTimelineEvents) && g_pdTimelineEngine.replan(next)) {
            // Timing, notes, x-fields or a mute changed: applied live. No restart, no device
            // close, no transport stop - muting PD must not touch the rest of the project.
            g_pdTimelineEvents=std::move(next);g_pdNeedsReload=false;return;
        }
        // A different set of PD clips: rebuild the PD engine only. The transport keeps going
        // (the VST engine takes the clock) and PD rejoins as soon as its new stream is ready.
        g_pdResumeAfterReload = g_timelineRun;
        if (g_pdTimelineMode) { pdTransport(false); g_pdTimelineMode=false; g_pdPlayingClip=0; g_resync=1; }
        g_pdTimelineEngine.stop();
        g_pdTimelineEngine.setHub(&g_cawHub);   // CAW hub: PD-only mode shares the same caws/cawr bus
        handAudioCardToNextStream();   // PD's engine thread opens its own stream: exclusive drivers need the card free
        if(!g_pdTimelineEngine.start(g_pdLibraryPath,next,pdAudioSettings())){
            g_pdEngineOn=false;g_pdNeedsReload=true;g_pdResumeAfterReload=false;
            playNotice("PD reload failed: "+g_pdTimelineEngine.status());
            restoreScSoloOutput();
            return;
        }
        g_pdSnapshotSeq=0;
        g_pdTimelineEvents=std::move(next);
        if (g_pdResumeAfterReload)
            playNotice("PD clips reloaded - PD rejoins when it is ready; the other engines keep playing.",
                       "PD reloading: the other engines keep playing");
    }
    g_pdNeedsReload=false;
}
#include "app.h"
void tickPdTimeline(float dt )
{
    if (dt > 0.f) refreshPdPlan(dt);
    if (!g_pdTimelineMode || !g_timelineRun) return;
    static std::uint64_t lastUnderruns=0;
    const auto underruns=g_pdTimelineEngine.underflows();
    if(underruns<lastUnderruns)lastUnderruns=0;
    if(underruns>lastUnderruns) {
        lastUnderruns=underruns;
        playNotice("PD audio underrun. Increase the audio buffer in Settings > Audio if glitches continue.",
                   "PD audio underrun: try a larger buffer");
    }
    const auto pos=g_pdTimelineEngine.position();
    if(pos>=0)g_play=static_cast<double>(pos)/std::max(1000,g_proj.cs.sr);
    if (g_vstEngineOn) vstSolo().followClock(g_play);   // the VST engine stays in time with PD
    const auto* event = pd_timeline::at(g_pdTimelineEvents, g_play);
    g_pdPlayingClip=(event && !event->muted)?event->clipId:0;   // a muted clip is loaded, not playing
    // MIDI edits / live M / S of the instrument tracks while PD drives the clock: the VST
    // engine gets its new schedule here instead of on the Csound path.
    g_sigT += dt;
    if (g_sigT >= 0.25) {
        g_sigT = 0.0;
        const unsigned long long sig = timelineSignature(g_proj);
        if (sig != g_sig) {
            g_sig = sig;
            if (g_vstEngineOn) vstSolo().setNotes(buildVstSoloNotes());
        }
    }
    if (g_scReady) {
        const auto sig = scTimelineSignature(g_proj);
        if (g_scTimelineSig != sig || (g_scLastPDTimelineTime >= 0.0 && g_play + 0.05 < g_scLastPDTimelineTime))
            scheduleScTimeline(g_play, true);   // PD-driven playhead jumped / wrapped: restart the windows
        else scTimelineWindows(g_play);         // the playhead moved into a new clip window
        pollScAutomation();   // SC automation lanes follow the PD playhead too
    }
    scheduleAudioTimeline(g_play, g_scLastPDTimelineTime >= 0.0 && g_play + 0.05 < g_scLastPDTimelineTime);   // audio voices follow the PD clock too
    g_scLastPDTimelineTime = g_play;
}
#include "app.h"
void runClipNow(const Clip& clip);
#include "app.h"
bool pushClipCodeToEngine(const Clip& clip);   // live code: recompile the current text into the running engine
#include "app.h"
void runClip(const Clip& clip) {
    if (!g_langCsound) { logLine("[caw] Enable Csound in Settings > Engines to run Csound clips."); return; }
    Clip copy = clip;
    logLine("[caw] Run Csound clip " + clip.name);
    runClipNow(copy);
}
#include "app.h"
void runClipNow(const Clip& clip) {
    if (g_renderActive) { logLine("[caw] A render is running - wait for it or cancel it first."); return; }
    if (engineIsUp()) {   // "Run" is running: play the whole clip in that engine
        const ParsedTrack pc = parseClip(clip);
        pushClipCodeToEngine(clip);   // the text may have changed since this engine was compiled
        if (auto it = g_liveCodeEdits.find(clip.id); it != g_liveCodeEdits.end()) it->second.pending = false;
        g_clipCodeDirty.erase(clip.id);   // SUPERCOLLIDER-STYLE: Run pushes the code, so it is no longer dirty
        // SUPERCOLLIDER-STYLE: Run REPLACES whatever is playing - stop the previous Run clip (a different
        // instrument still inside its duration) and any ringing tail of this clip's own instrument, then start fresh.
        if (g_runLeft > 0.0 && !g_runName.empty() && g_runName != clip.name) {
            bool stoppedPrev = false;
            for (const Track& t : g_proj.tracks) {
                for (const Clip& c : t.clips) if (c.name == g_runName) { commandStopClip(c); stoppedPrev = true; break; }
                if (stoppedPrev) break;
            }
            g_runLeft = 0.0; g_runName.clear();
        }
        commandStopClip(clip);   // kill a still-ringing instance of this instrument (release tail) so the new Run is clean
        if (numberOf(g_engClips, clip.name)) { commandStartClip(clip, clip.dur, pc.midiEnabled ? 60.0 : 0.0, pc.midiEnabled ? 100.0 : 0.0); g_runName = clip.name; g_runLeft = clip.dur; }
        if (g_runLeft > 0.0 && g_runName == clip.name) logLine("[caw] Run clip " + clip.name + ": playing the whole clip (" + std::to_string(clip.dur) + " s) in the running Csound.");
        else logLine("[caw] Could not send the clip to Csound.");
        return;
    }
    // DSP OFF: Run plays the clip. Start the DSP engine; pollProcesses fires the pending clip
    // (whole duration, audible) as soon as the engine is up. Compile errors still appear in the
    // console - the engine compiles the CSD on its way up.
    g_testProc.stop();
    g_test = TestRun();
    g_csoundPendingClipId = clip.id;
    g_csoundPendingAt = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (g_liveCsound) {
        playNotice("The DSP is starting. The clip plays when the engine is ready.", "DSP is starting");
    } else {
        logLine("[caw] Run clip " + clip.name + ": the DSP is off - starting it; the clip plays when the engine is up.");
        startEngine();
    }
}
#include "app.h"
void stopClipRun(const Clip& clip) {
    if (g_runLeft > 0.0 && g_runName == clip.name) {
        commandStopClip(clip);
        g_runLeft = 0.0;
    }
    if (g_testProc.active() && g_test.name == clip.name) g_testProc.stop();
    logLine("[caw] Run clip " + clip.name + ": stopped.");
}
#include "app.h"
bool pushClipCodeToEngine(const Clip& clip) {
    if (!engineIsUp()) return false;
    if (!g_useCsoundApi) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            logLine("[caw] Live code needs CAW's Csound API engine (the external Csound process cannot recompile while it runs). Code edits are applied by the next Run.");
        }
        return false;
    }
    const std::string orc = buildClipInstrumentCode(g_proj, clip);
    if (orc.empty()) {
        logLine("[caw] live code \"" + clip.name + "\": this clip has no instrument in the running engine - press Run once.");
        return false;
    }
    g_apiProc.pushLiveInstrument(clip.name, orc);
    return true;
}
#include "app.h"
void restartClipForLiveCode(const Clip& clip, const Track& track) {
    if (!engineIsUp()) return;
    const bool manual = g_runLeft > 0.0 && g_runName == clip.name;   // started with the clip's Run button
    double dur = clip.dur;
    if (!manual) {
        if (!g_playing || !g_timelineRun || clip.muted || track.muted) return;
        bool anySolo = false;
        for (const Track& t : g_proj.tracks) if (t.solo && !t.muted) anySolo = true;
        if (anySolo && !track.solo) return;
        const double local = g_play - clip.start;
        if (local < 0.0 || local >= clip.dur) return;
        const ParsedTrack pc = parseClipForTrack(clip, track);
        const int runs = pc.launcher ? 1 : std::min(256, std::max(1, clip.runs));
        const double slot = clip.dur / (double)runs;
        dur = slot - std::fmod(local, slot);
        if (dur <= 0.05) return;
    }
    commandStopClip(clip);
    commandStartClip(clip, dur, 0, 0);
    if (manual) g_runLeft = dur;
}
#include "app.h"
void applyLiveClipCode(const Clip& clip, const Track& track) {
    if (pushClipCodeToEngine(clip)) restartClipForLiveCode(clip, track);
}
#include "app.h"
void applyScClip(const Clip& clip) {
    // Apply = the code becomes what the timeline (and any relaunch) uses from now on.
    g_scTimelineCode[clip.id] = clip.code;
    const bool inWindow = g_timelineRun && clip.start <= g_play + 1e-9 && g_play < clip.start + clip.dur;
    // DSP OFF: Run plays the clip (it boots the SuperCollider engine and waits for it - launchScClip
    // handles the pending start). DSP ON: Run is an APPLY button only - the timeline re-evaluates a
    // playing window from the new code snapshot; nothing is auditioned, with the transport running
    // or stopped.
    if (!g_scReady && !g_scEngineOn) {
        if (!(g_timelineRun && inWindow)) {   // the timeline is already sounding this window: applying is enough
            if (scClipRunning(clip.id)) stopScClip(clip.id);   // a fresh audition with the new code
            launchScClip(clip);
            g_scManualRuns[clip.id] = std::chrono::steady_clock::now() +
                std::chrono::milliseconds((long long)(clip.dur * 1000.0 + 500.0));   // the button shows it (manual audition)
        }
    } else if (g_timelineRun && inWindow && scClipRunning(clip.id)) {
        stopScClip(clip.id);
        launchScClip(clip, clip.start + clip.dur - g_play);   // apply live: re-evaluate for the remaining window
    }
    g_clipCodeDirty.erase(clip.id);
}
#include "app.h"
bool applyClipCodeNow(const Clip& clip, const Track& lane) {
    if (lane.kind == 5) {           // SuperCollider: apply the current code (Run / Alt+Enter)
        if (g_scReady || g_scEngineOn) { applyScClip(clip); return true; }
        logLine("[caw] Enable the SuperCollider engine (Settings > Engines) to evaluate this clip.");
        return false;
    }
    if (lane.kind == 3 || lane.kind == 4) {   // SEPARATED: the VST engine interprets the edited clip code in C++
        if (g_vstEngineOn) { vstSolo().setScripts(buildVstScripts()); logLine("[caw] live code: the VST engine is using the edited clip code."); g_clipCodeDirty.erase(clip.id); return true; }
        // DSP OFF: Run starts the VST engine - it loads with the edited code (the jobs read the
        // current clip text at start), so the clip sounds through Play / its MIDI notes.
        logLine("[caw] The VST engine was off - it is starting with the edited clip code.");
        g_clipCodeDirty.erase(clip.id);
        startVstEngine();
        return true;
    }
    // Csound: push the new instrument into the running engine (and restart the clip at the playhead).
    if (engineIsUp()) { applyLiveClipCode(clip, lane); g_clipCodeDirty.erase(clip.id); return true; }
    runClip(clip);                  // no running engine: run the clip alone so its errors print in the console
    g_clipCodeDirty.erase(clip.id);
    return true;
}
#include "app.h"
bool resolveOrcLine(const std::string& l, std::string& text, std::string& name, int& src) {
    static const std::regex lineRe(R"re(\bline\s+(\d+))re", std::regex_constants::icase);
    std::smatch m;
    if (g_orcSpans.empty() || l.find("score") != std::string::npos || !std::regex_search(l, m, lineRe)) return false;
    const int orcLine = std::atoi(m[1].str().c_str());
    for (const OrcSpan& sp : g_orcSpans) {
        const int idx = orcLine - sp.first;
        if (idx < 0 || idx >= (int)sp.lineMap.size()) continue;
        src = sp.lineMap[(size_t)idx];
        name = sp.name;
        const std::string where = "instr " + sp.name + ", " + (src > 0 ? "clip line " + std::to_string(src) : std::string("generated line (<caw> / cawRun)"));
        text = l.substr(0, (size_t)m.position(0)) + where + l.substr((size_t)(m.position(0) + m.length(0)));
        return true;
    }
    return false;
}
#include "app.h"
void pollTestAndScan(float dt) {
    if (g_runLeft > 0.0) g_runLeft -= dt;
    if (g_pdRunLeft > 0.0) { g_pdRunLeft -= dt; if (g_pdRunLeft <= 0.0) { g_pdRunLeft = 0.0; if (g_pdPreview.active()) stopPdPatch(); } }   // SUPERCOLLIDER-STYLE: a PD Run-clip stops itself at the end of its duration
    if (g_testProc.active()) {
        for (const std::string& l : g_testProc.takeOutput()) {
            std::string low = l;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (low.find("error") != std::string::npos || low.find("failed") != std::string::npos) g_test.sawError = true;
            logCsoundLine(l);
        }
        if (g_testProc.finished()) {
            const int code = g_testProc.exitCode();
            g_testProc.stop();
            logLine(g_test.sawError || code != 0 ? "[caw] Run clip " + g_test.name + ": errors found, see above."
                                                  : "[caw] Run clip " + g_test.name + ": no errors.");
        }
    }
    if (g_opcodeProc.active()) {
        g_opcodeScanT += dt;
        static const std::regex word(R"re(^\s*([A-Za-z_]\w*)\b)re");
        for (const std::string& l : g_opcodeProc.takeOutput()) {
            std::smatch m;
            if (std::regex_search(l, m, word)) addExtraOpcode(m[1].str());
        }
        if (g_opcodeProc.finished() || g_opcodeScanT > 10.f) g_opcodeProc.stop();
    } else if (g_langCsound && !g_opcodeScanStarted && (g_textEditorFocused || g_playing)) {
        g_opcodeScanStarted = true;
        g_opcodeProc.start("\"" + csoundExePath() + "\" -z", false);   // only when code/engine needs it
    }
}
#include "app.h"
void pollProcesses(float dt) {
    // Load the engines' shared runtime DLLs on the first frame, before any PD preview, audition or engine
    // start can bring its own copies in: Windows keeps one copy per DLL name and whoever loads first decides
    // for the whole process. This is what made Csound + PD fail after PD clips had been previewed earlier in
    // the same session - Csound's plugins then got libpd's copies, or found no dependencies at all.
    static bool sharedOnce = false;
    if (!sharedOnce) { sharedOnce = true; shareEngineDlls(); }
    if (g_csoundPendingClipId > 0) {   // a Run pressed while the DSP was off: play the clip once the engine is up
        if (engineIsUp()) {
            const int pending = g_csoundPendingClipId;
            g_csoundPendingClipId = 0;
            if (Clip* clip = findClip(pending)) runClipNow(*clip);
            else logLine("[caw] The pending Run clip no longer exists.");
        } else if (std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - g_csoundPendingAt > 20.0) {
            g_csoundPendingClipId = 0;
            logLine("[caw] The DSP did not come up - the pending Run clip was cancelled (see the engine log above).");
        }
    }
    pollScEngine(dt);
    drainPdControlOutputs();
    pollCawHub();   // deliver caws/cawr hub changes to the SuperCollider language
    // ---- Play (Csound runs the whole project in real time)
    const bool usingApi = g_useCsoundApi && g_apiProc.active();
    if (usingApi || g_playProc.active()) {
        if (g_playing && g_proj.cs.monitorFeed && !g_monSeen && !g_monWarned) {
            g_monRunT += dt;
            if (g_monRunT > 5.f) {
                g_monWarned = true;
                logLine("[caw] No monitor data from Csound after 5 s. Look for a Csound error above (CawMon); you can untick 'Send meter data' in Settings.");
            }
        }
        const auto playLines = usingApi ? g_apiProc.takeOutput() : g_playProc.takeOutput();
        for (const std::string& l : playLines) {
            if (handleMonitorLine(l)) { g_sectionSeen = true; continue; }
            logCsoundLine(l);
            // "Live" = Csound is really performing. API engine: its first audio block reached the soundcard ("SECTION 1" is printed
            // earlier, before CAW opened the soundcard, so it must not count there). Process engine: Csound's own "SECTION 1" line,
            // which it prints after it has opened the soundcard.
            if (usingApi) { if (l.find("first audio block delivered") != std::string::npos) g_sectionSeen = true; }
            else if (l.find("SECTION 1") != std::string::npos) g_sectionSeen = true;
            {   // the sound card could not be opened: Csound never starts playing
                std::string low = l;
                std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                static bool hinted = false;
                if (!g_playWaiting && !g_sectionSeen) hinted = false;
                if (!hinted && (low.find("unable to open") != std::string::npos || low.find("error opening") != std::string::npos || low.find("portaudio error") != std::string::npos ||
                                low.find("failed to initialise") != std::string::npos || low.find("failed to initialize") != std::string::npos || low.find("audio device") != std::string::npos)) {
                    hinted = true;
                    logLine("[caw] Csound could not open the sound card at " + std::to_string(g_proj.cs.sr) + " Hz. Set the sample rate in Settings to the rate of your output device (usually 48000 or 44100) and the right output device, then press Run again.");
                }
            }
        }
        if (usingApi) {
            MonitorCapture::Snapshot snapshot;
            if (g_apiProc.takeSnapshot(snapshot, g_apiSnapshotSeq)) {
                g_apiSnapshotSeq = snapshot.sequence;
                // The API host captures after CAW mixes SC/PD, so samples are
                // already normalized to the device range.
                g_rawFullScale = 1.f;
                const auto produced = std::chrono::steady_clock::time_point(
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(snapshot.timestampNs)));
                const auto due = produced + std::chrono::microseconds((long long)(engineOutputLatency() * 1e6));
                MonFrame raw;
                raw.tag = 'B';
                raw.values.reserve(monitor_fft::size * 2);
                raw.values.insert(raw.values.end(), snapshot.left.begin(), snapshot.left.end());
                raw.values.insert(raw.values.end(), snapshot.right.begin(), snapshot.right.end());
                g_monHeld.emplace_back(due, std::move(raw));
                MonFrame meters;
                meters.tag = 'M';
                meters.values = std::move(snapshot.meters);
                g_monHeld.emplace_back(due, std::move(meters));
            }
        }
        pollMonitor();
        writeSerial();
        if (g_playWaiting) {
            g_playWaitT += dt;
            // Live only when Csound really started performing (its "SECTION 1" line, the API start message or the first meter data).
            // Opening the sound card (ASIO / WDM-KS ...) can take several seconds, so there is no short timer any more.
            // Fallback after 15 s for a Csound with silenced messages (-m0): go live, but only while the process is still running.
            if (g_sectionSeen) g_playWaiting = false;
            else if (g_playWaitT > 15.f && (usingApi ? g_apiProc.running() : g_playProc.running())) {
                g_playWaiting = false;
                logLine("[caw] Csound did not report that it started (messages off?). Assuming it is running.");
            }
        }
        const bool finished = usingApi ? g_apiProc.finished() : g_playProc.finished();
        if (finished) {
            const int code = usingApi ? g_apiProc.exitCode() : g_playProc.exitCode();
            if (usingApi) g_apiProc.stop(); else g_playProc.stop();
            drainPlayOutput();
            closeMonitorSocket();
            g_cawMonitorApi = false;
            g_useCsoundApi = false;
            g_playing = false;
            g_liveCsound = false;
            g_mixedPd = false;
            g_playWaiting = false;
            restoreScSoloOutput();   // the VST engine (and SC) get the soundcard back
            logLine(code == 0 ? "[caw] Csound stopped." : "[caw] Csound stopped with exit code " + std::to_string(code) + " - see messages above.");
            if (code != 0 && (g_rtMissingAbs || g_rtMissingSqrt || g_rtMissingPow)) {
                // Self-heal: learn ONLY from Csound's own message, then adapt the generator so the
                // next Run compiles (abs -> sqrt(x*x), sqrt -> raw sums, pow -> sqrt).
                if (g_rtMissingAbs && g_mathHasAbs) {
                    g_mathHasAbs = false;
                    logLine("[caw] This Csound has no usable abs opcode - meters will be built with sqrt(x*x). Press DSP again.");
                }
                if (g_rtMissingSqrt && g_mathHasSqrt) {
                    g_mathHasSqrt = false;
                    logLine("[caw] This Csound has no usable sqrt opcode - spectrum bands will use raw sums. Press DSP again.");
                }
                if (g_rtMissingPow && g_mathHasPow) {
                    g_mathHasPow = false;
                    logLine("[caw] This Csound has no usable pow opcode - spectrum bands will use sqrt. Press DSP again.");
                }
                setCsoundMathOps(g_mathHasAbs, g_mathHasSqrt, g_mathHasPow);
            }
            if (usingApi && code != 0)
                logLine("[caw] If the API engine is incompatible with your Csound installation, use run_osc_monitor.bat to try the previous engine.");
        }
    }

    if (g_pdEngineOn && !g_pdTimelineEngine.active()) {
        for(const auto& note:g_pdTimelineEngine.takeNotes())logPdNote(note);
        const std::string status = g_pdTimelineEngine.status();
        if (status != "PD stopped.") logPdFailureStatus(status);
        playNotice("PD engine stopped: " + status, "PD stopped: see the log");
        if (g_scReady) stopAllScClips();
        g_pdEngineOn = false; g_pdTimelineMode = false; g_pdPlayingClip = 0;
        g_pdResumeAfterReload = false;
        if (g_timelineRun) g_resync = 1;   // the VST / SC clock takes over from this position
        g_pdTimelineEvents.clear();
        restoreScSoloOutput();
    }
    vstui::captureParameters(g_proj);   // EVERY frame: the editor idle tick (plugin GUIs: EQ waves, synth meters, Kontakt keys) and the live editor-state poll (Kontakt's .nki -> engine). Without this they only ran on save / engine start / reload / export.
    vstEngineTick(dt);   // VST engine: log, own stream, plugin changes
    // The PD engine was rebuilt while the transport kept running (a different set of PD
    // clips). Rejoin as soon as its new stream is ready; PD then drives the clock again
    // and the VST engine re-anchors on it.
    if (g_timelineRun && g_pdResumeAfterReload && !g_pdTimelineMode && g_pdEngineOn &&
        g_pdTimelineEngine.ready() && !g_renderActive) {
        std::vector<pd_timeline::Event> plan; std::string err;
        if (!makePdPlan(plan, err, true) || !pd_timeline::samePlan(plan, g_pdTimelineEvents)) {
            g_pdResumeAfterReload = false;
        } else {
            g_pdResumeAfterReload = false;
            g_pdTimelineMode = true;
            g_pdPreviousTime = -1.0;
            vstEngineJoinTransport();
            pdTransport(true);
            logLine("[caw] PD rejoined the running transport.");
        }
    }
    // ---- timeline (independent of the Csound engine)
    if (g_timelineRun) {
        if (g_pdTimelineMode) {
            if(g_drag!=Drag::Playhead)tickPdTimeline(dt);
        } else tickTimeline(dt);
    }
    else refreshPdPlan(dt);
    refreshMixedPdPlan(dt);

    // ---- CAW offline renderer, followed by a codec only for FLAC/MP3
    auto finishExport = [&](const std::string& file) {
        std::string error;
        std::error_code ec;
        const bool appeared = !g_renderWillOverwrite && std::filesystem::exists(g_renderPath, ec);
        if (ec) error = "Could not check the destination: " + ec.message();
        else if (appeared) error = "The destination appeared during rendering; it was not replaced.";
        else if (replaceOutputFile(file, g_renderPath, error)) logLine("[caw] Audio export finished: " + g_renderPath);
        if (!error.empty()) { g_exportError = error; logLine("[caw] Audio export failed: " + error); }
        g_renderActive = false;
        cleanupRenderTemps();
    };
    // ---- real-time whole-project export (Render mode = real-time): the transport rolls, the
    // callback copies the final master mix; when the playhead passes the end the capture drains,
    // the WAV closes and the encode/finish machinery below takes over.
    if (g_renderActive && g_exportRt.phase == 4) {   // arming: wait for the started engines + the soundcard
        const bool csoundUp = !g_langCsound || engineIsUp();
        const bool scUp = !g_langSc || g_scReady;
        const bool vstUp = !g_langVst || g_vstEngineOn;
        const bool streamUp = g_sampleOutput || g_scSoloOutput || g_vstSoloOutput || g_apiProc.active() || g_pdEngineOn;
        const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_exportRt.startedAt).count();
        if (waited > 25.0) {
            g_exportError = "The real-time export could not start its engines in time - start them with DSP and try again.";
            logLine("[caw] " + g_exportError);
            exportRtCancel();
            g_renderActive = false;
            cleanupRenderTemps();
        } else if (csoundUp && scUp && vstUp && streamUp) {
            // The WAV opens NOW: the capture rate must be the device's ACTUAL rate (the sample
            // engine follows it, round 159) - opening earlier could mismatch and pitch the file.
            g_exportRt.rate = activeStreamRate(std::max(8000, g_proj.cs.sr));
            std::string werr;
            if (!g_exportRt.wav.open(g_exportRt.tmpPath, g_exportRt.rate, g_exportRt.ch, g_exportRt.wavFormat, werr)) {
                g_exportError = "The real-time export could not create its WAV: " + werr;
                logLine("[caw] " + g_exportError);
                exportRtCancel();
                g_renderActive = false;
                cleanupRenderTemps();
                return;
            }
            FreezeCaptureSink* sink = &g_exportRt.sink;
            const int ch = g_exportRt.ch;
            g_exportRt.writer = std::thread([sink, ch]() {
                std::vector<float> block((size_t)2048 * (size_t)ch);
                std::string werr;
                for (;;) {
                    if (sink->pop(block.data(), 2048, ch)) {
                        if (sink == &g_exportRt.sink && !g_exportRt.wav.write(block.data(), 2048, 1.0, 1.f, werr))
                            g_exportRt.writeFailed.store(1, std::memory_order_release);
                        continue;
                    }
                    if (g_exportRt.stopWriter.load(std::memory_order_acquire)) {
                        while (sink->pop(block.data(), 256, ch))
                            if (sink == &g_exportRt.sink && !g_exportRt.wav.write(block.data(), 256, 1.0, 1.f, werr))
                                g_exportRt.writeFailed.store(1, std::memory_order_release);
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            });
            g_exportSink.store(sink, std::memory_order_release);
            sink->active.store(true, std::memory_order_release);
            g_exportRt.phase = 1;
            g_exportRt.captureStartedAt = std::chrono::steady_clock::now();
            g_exportRt.lastPlay = g_play;
            g_proj.loopEnabled = false;
            seekTimeline(g_exportRt.t0);
            toggleTimelinePlay();
            logLine("[caw] Real-time export: the engines are up - the transport rolls at " + std::to_string(g_exportRt.rate) + " Hz.");
        }
    }
    if (g_renderActive && g_exportRt.phase == 1) {
        const double sinceStart = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_exportRt.captureStartedAt).count();
        if (g_exportRt.writeFailed.load(std::memory_order_acquire)) {
            g_exportError = "The real-time export could not write its WAV (disk problem).";
            logLine("[caw] " + g_exportError);
            exportRtCancel();
            g_renderActive = false;
            cleanupRenderTemps();
        } else if (!timelinePlaying() && g_play < g_exportRt.t0 + 0.5) {
            // The transport refused to start (toggleTimelinePlay can answer with a notice instead
            // of playing, e.g. shared engines still preparing). Wait briefly, then fail CLEARLY -
            // silently finishing here produced empty exports.
            if (sinceStart > 3.0) {
                g_exportError = "The real-time export could not start the transport (see the notice in the log). Stop other playback and try again.";
                logLine("[caw] " + g_exportError);
                exportRtCancel();
                g_renderActive = false;
                cleanupRenderTemps();
            }
        } else if (!timelinePlaying() || g_play >= g_exportRt.t1 ||
                   sinceStart > (g_exportRt.t1 - g_exportRt.t0) + 6.0) {
            // Hard deadline: the capture can never take longer than its own span plus margin.
            // A playhead that parks at the end (or any transport oddity) used to keep the export
            // running forever - "the first try is stuck at the end". With the deadline it always
            // finishes and keeps whatever was captured.
            if (timelinePlaying()) toggleTimelinePlay();
            g_exportRt.sink.quiesce();                       // the callback stops copying first
            g_exportSink.store(nullptr, std::memory_order_release);
            g_exportRt.phase = 2;
            logLine("[caw] Real-time export: capture done at " + std::to_string(g_play) + " s, writing the file...");
        } else {
            // The transport rolls but the playhead must move: a stalled clock would otherwise
            // keep the export running forever (the "pause forever" report).
            if (std::fabs(g_play - g_exportRt.lastPlay) > 1e-9) {
                g_exportRt.lastPlay = g_play;
                g_exportRt.captureStartedAt = std::chrono::steady_clock::now();
            } else if (sinceStart > 10.0) {
                g_exportError = "The real-time export stopped: the playhead stopped advancing. Check the engines (DSP) and try again.";
                logLine("[caw] " + g_exportError);
                exportRtCancel();
                g_renderActive = false;
                cleanupRenderTemps();
            }
        }
    }
    if (g_renderActive && g_exportRt.phase == 2) {
        if (g_exportRt.sink.queued() == 0) {
            g_exportRt.stopWriter.store(true, std::memory_order_release);
            if (g_exportRt.writer.joinable()) g_exportRt.writer.join();
            std::string werr;
            const bool ok = g_exportRt.wav.finish(werr);
            g_play = g_exportRt.savedPlayhead;
            g_proj.loopEnabled = g_exportRt.savedLoopEnabled;
            g_proj.loopStart = g_exportRt.savedLoopStart;
            g_proj.loopEnd = g_exportRt.savedLoopEnd;
            g_exportRt.phase = 0;
            if (g_exportRt.wav.dataBytes() == 0) {   // an empty capture must never "finish" as a silent file
                g_exportError = "The real-time export captured nothing - the engines were up but no audio reached the capture. Check DSP / the engines and try again.";
                logLine("[caw] " + g_exportError);
                g_renderActive = false;
                cleanupRenderTemps();
            } else if (!ok || g_exportRt.writeFailed.load(std::memory_order_acquire)) {
                g_exportError = "The real-time export WAV could not be written" + std::string(werr.empty() ? "." : ": " + werr);
                logLine("[caw] " + g_exportError);
                g_renderActive = false;
                cleanupRenderTemps();
            } else {
                logLine("[caw] Real-time export finished in " + std::to_string((int)(std::chrono::duration<double>(std::chrono::steady_clock::now() - g_exportRt.startedAt).count() + 0.5)) + " s.");
                if (g_activeRenderFormat >= 3) {
                    g_renderConverting = true;
                    logLine(g_activeRenderFormat == 3 ? "[caw] Encoding FLAC..." : "[caw] Encoding MP3...");
                    // The bundled ffmpeg first (it is always there); the native Csound encoder only if it is missing.
                    if (!startFallbackEncoder() &&
                        !g_codecProc.start(g_renderTemp, g_renderEncodedTemp, csoundExePath(),
                                           g_activeRenderFormat, g_activeFlacBits, g_activeMp3Kbps)) {
                        g_exportError = "Audio encoder could not start.";
                        logLine("[caw] " + g_exportError);
                        g_renderActive = false; cleanupRenderTemps();
                    }
                } else finishExport(g_renderTemp);
            }
        }
    }
    // The generic export polling must stay out of the way while a freeze pass runs: the per-engine
    // offline export drives its parts THROUGH the freeze machinery, which uses this same host.
    if (g_renderActive && g_freeze.phase == 0 && g_exportRt.phase == 0 && g_offlineProc.active()) {
        for (const std::string& l : g_offlineProc.takeOutput()) logCsoundLine(l);
        if (g_offlineProc.finished()) {
            const int code = g_offlineProc.exitCode();
            g_offlineProc.stop();
            for (const std::string& l : g_offlineProc.takeOutput()) logLine(l);
            if (code != 0) {
                g_exportError = "Audio render failed. Check the console for details.";
                logLine("[caw] Offline audio export failed (code " + std::to_string(code) + ").");
                g_renderActive = false; cleanupRenderTemps();
            } else if (g_activeRenderFormat >= 3) {
                g_renderConverting = true;
                logLine(g_activeRenderFormat == 3 ? "[caw] Encoding FLAC..." : "[caw] Encoding MP3...");
                // The bundled ffmpeg first (it is always there); the native Csound encoder only if it is missing.
                if (!startFallbackEncoder() &&
                    !g_codecProc.start(g_renderTemp, g_renderEncodedTemp, csoundExePath(),
                                       g_activeRenderFormat, g_activeFlacBits, g_activeMp3Kbps)) {
                    g_exportError = "Audio encoder could not start.";
                    logLine("[caw] " + g_exportError);
                    g_renderActive = false; cleanupRenderTemps();
                }
            } else finishExport(g_renderTemp);
        }
    }
    if (g_codecProc.active() && g_codecProc.finished()) {
        const bool success = g_codecProc.success();
        const std::string error = g_codecProc.error();
        g_codecProc.stop();
        if (success) finishExport(g_renderEncodedTemp);
        else {
            logLine("[caw] Native encoder: " + error);
            if (!startFallbackEncoder()) {
                g_exportError = "MP3/FLAC encoder unavailable. On Windows, keep the bundled ffmpeg.exe beside CAW; otherwise install FFmpeg on PATH. Native encoder: " + error;
                logLine("[caw] " + g_exportError);
                g_renderActive = false; cleanupRenderTemps();
            }
        }
    }
    if (g_renderProc.active()) {
        for (const std::string& l : g_renderProc.takeOutput()) logLine(l);
        if (g_renderProc.finished()) {
            const int code = g_renderProc.exitCode();
            g_renderProc.stop();
            for (const std::string& l : g_renderProc.takeOutput()) logLine(l);
            if (code != 0) {
                g_exportError = "Audio encoding failed. Check the console for details.";
                logLine("[caw] Audio encoding failed (code " + std::to_string(code) + ").");
                g_renderActive = false; cleanupRenderTemps();
            } else finishExport(g_renderEncodedTemp);
        }
    }
}
#include "app.h"
void runJoinTool() {
    if (!g_selMidiClips.empty()) {
        joinSelectedClips();
        if (g_tool == Tool::Join) g_tool = Tool::Select;
    } else if (audioClipsSelected()) {
        joinAudioSelectedClips();   // audio clips join by merging their source segments
        if (g_tool == Tool::Join) g_tool = Tool::Select;
    } else {
        g_tool = Tool::Join;
    }
}
#include "app.h"
std::string engineSettingsToText(const CsoundSettings& s) {
    std::ostringstream o;
    o << (s.options.empty() ? defaultOptions(s) : s.options) << "\n";
    o << "sr = " << s.sr << "\nksmps = " << s.ksmps << "\nnchnls = " << s.nchnls << "\n";
    if (s.nchnlsIn > 0) o << "nchnls_i = " << s.nchnlsIn << "\n";
    o << "0dbfs = " << s.zdbfs << "\nA4 = " << s.a4 << "\n";
    return o.str();
}
#include "app.h"
bool parseEngineLine(const std::string& raw, std::string& key, double& value) {
    std::string line = raw.substr(0, raw.find(';'));
    auto trim = [](std::string v) {
        const size_t a = v.find_first_not_of(" \t\r");
        if (a == std::string::npos) return std::string();
        return v.substr(a, v.find_last_not_of(" \t\r") - a + 1);
    };
    line = trim(line);
    if (line.empty()) return false;
    const size_t split = line.find('=');
    if (split == std::string::npos) return false;
    key = trim(line.substr(0, split));
    std::string rest = trim(line.substr(split + 1));
    if (!rest.empty() && rest[0] == '=') rest = trim(rest.substr(1));
    char* end = nullptr;
    value = std::strtod(rest.c_str(), &end);
    return end && end != rest.c_str() && *end == '\0';
}
#include "app.h"
std::string engineTextProblems(const std::string& text) {
    static const std::set<std::string> known = {"sr", "ksmps", "kr", "nchnls", "nchnls_i", "0dbfs", "A4"};
    std::istringstream in(text);
    std::string raw, key, out;
    double v = 0.0;
    int lineNo = 0;
    while (std::getline(in, raw)) {
        ++lineNo;
        const std::string code = raw.substr(0, raw.find(';'));
        if (code.find_first_not_of(" \t\r") == std::string::npos) continue;
        if (isOptionLine(raw)) continue;
        if (!parseEngineLine(raw, key, v)) out += "line " + std::to_string(lineNo) + ": expected  name = number  or a Csound option starting with -\n";
        else if (!known.count(key)) out += "line " + std::to_string(lineNo) + ": unknown setting '" + key + "'\n";
    }
    return out;
}
#include "app.h"
const TextEditor::LanguageDefinition& csoundLanguage() {
    static TextEditor::LanguageDefinition lang;
    static int built = 0;
    if (built == g_langVersion) return lang;
    built = g_langVersion;
    lang = TextEditor::LanguageDefinition::CPlusPlus();
    lang.mName = "Csound";
    lang.mSingleLineComment = ";";
    lang.mKeywords.clear();      // the C++ keywords / functions do not belong here
    lang.mIdentifiers.clear();
    for (const char* keyword : {"instr", "endin", "opcode", "endop", "if", "then", "elseif", "else", "endif", "ithen", "kthen",
                                "while", "until", "do", "od", "goto", "igoto", "kgoto", "tigoto", "rigoto", "cggoto", "cigoto", "ckgoto",
                                "cngoto", "cnkgoto", "timout", "reinit", "rireturn", "return", "this", "caw", "cawget"})
        lang.mKeywords.insert(keyword);
    TextEditor::Identifier known;
    known.mDeclaration = "Csound opcode";
    for (const char* op : kCsoundOpcodes) lang.mIdentifiers.insert(std::make_pair(std::string(op), known));
    for (const char* op : {"cawMath", "cawRun", "dawrun", "cawset", "cawfile"}) lang.mIdentifiers.insert(std::make_pair(std::string(op), known));
    for (const std::string& op : g_extraOpcodes) lang.mIdentifiers.insert(std::make_pair(op, known));
    // first in the list = wins over the generic identifier rule
    lang.mTokenRegexStrings.insert(lang.mTokenRegexStrings.begin(), {"p[0-9]+\\b", TextEditor::PaletteIndex::PreprocIdentifier});
    lang.mTokenRegexStrings.insert(lang.mTokenRegexStrings.begin(), {"<\\/?caw>", TextEditor::PaletteIndex::Preprocessor});
    return lang;
}
#include "app.h"
std::string audioVariableName(const Clip& clip, const AudioAsset& asset) {
    std::string base = "aPool_";
    for (unsigned char c : asset.name)
        base.push_back((std::isalnum(c) || c == '_') ? (char)c : '_');
    if (base == "aPool_") base += "Sample";
    std::string name = base;
    int suffix = 2;
    while (std::regex_search(clip.code, std::regex("\\b" + name + "\\b")))
        name = base + std::to_string(suffix++);
    return name;
}
#include "app.h"

// ---------------------------------------------------------------- TRACK FREEZE (Cubase-style lock)
// State lives in track_freeze.h; the definitions live here as plain functions (declared in
// app_internal.h, called from app.cpp / ui_timeline.cpp / tickAutosave). They must NOT be inline
// in the header: MSVC discards an inline definition that its only including TU never calls.

bool freezeActive() { return g_freeze.phase != 0; }

bool freezeOffline() { return g_freeze.phase == 3; }

float freezeProgress() {
    if (g_freeze.phase == 2) return 1.f;
    if (g_freeze.phase == 3) {   // the offline host gives no fraction: show an indeterminate sweep
        const double t = std::fmod(ImGui::GetTime() * 0.55, 1.0);
        return (float)(t < 0.5 ? t * 2.0 : (1.0 - t) * 2.0);
    }
    if (g_freeze.phase != 1 || g_freeze.t1 <= g_freeze.t0) return 0.f;
    const double p = (g_play - g_freeze.t0) / (g_freeze.t1 - g_freeze.t0);
    return (float)std::max(0.0, std::min(1.0, p));
}

const std::string& freezeTrackName() { return g_freeze.trackName; }

const std::string& freezeError() { return g_freeze.error; }

// Restores every mixer state the capture pass touched (mute / solo, loop, master FX, VSTe inputs,
// the target track's volume slider). Called on every exit path - success, cancel and error.
void freezeRestoreStates() {
    if (g_freeze.trackIdx < 0 || g_freeze.trackIdx >= (int)g_proj.tracks.size()) return;
    const size_t n = std::min(g_proj.tracks.size(), g_freeze.savedMuteSolo.size());
    for (size_t i = 0; i < n; ++i) {
        g_proj.tracks[i].muted = g_freeze.savedMuteSolo[i][0];
        g_proj.tracks[i].solo = g_freeze.savedMuteSolo[i][1];
    }
    size_t v = 0;
    for (Track& t : g_proj.tracks) {
        if (t.kind != 4) continue;
        if (v < g_freeze.savedVstInputs.size())
            for (int s = 0; s < 8; ++s) t.vstInputs[s].on = g_freeze.savedVstInputs[v][s];
        ++v;
    }
    g_proj.loopEnabled = g_freeze.savedLoopEnabled;
    g_proj.loopStart = g_freeze.savedLoopStart;
    g_proj.loopEnd = g_freeze.savedLoopEnd;
    g_proj.tracks[(size_t)g_freeze.trackIdx].volume = g_freeze.savedVolume;
    g_freezeSilent.store(false, std::memory_order_release);       // the render is over: the speakers come back
    g_masterChainEnabled.store(g_freeze.savedMasterChain, std::memory_order_relaxed);
    g_scRawSuppress.store(false, std::memory_order_release);      // the VSTe routing is back: the raw mix flows again
    g_sampleRawSuppress.store(false, std::memory_order_release);
    pushVstActiveLive();                                          // the same live refresh a mute / solo click does
}

// Stops the writer thread (it drains the ring first), closes the WAV and joins.
// The OFFLINE pass has no writer thread and no sink: the main thread wrote the WAV directly -
// the WAV must still be closed here (an open std::ofstream would make the rename into place fail
// with a sharing violation, and the next pass's open() would hit an already-open stream).
void freezeStopWriter(bool finishWav) {
    g_freezeSink.store(nullptr, std::memory_order_release);       // the callback stops copying first
    g_freeze.sink.quiesce();                                      // wait for any callback that already entered copy()
    g_freeze.stopWriter.store(true, std::memory_order_release);
    if (g_freeze.writer.joinable()) g_freeze.writer.join();       // the realtime capture: drain the ring, join the writer
    std::string err;
    if (finishWav) {
        if (!g_freeze.wav.finish(err)) g_freeze.error = err;      // patches the header and closes the file
    } else {
        g_freeze.wav.close();                                     // the pass is discarded: just close the file
    }
}

// The folder CAW saves its generated files in, one folder per kind ("lkd" = lock renders; every
// kind gets its own folder inside exports/). Two storage choices (Settings > Files > File storage):
//   "CAW folder (user directory)"  -> C:\Users\<name>\caw\exports\<kind> on Windows,
//                                     ~/caw/exports/<kind> on macOS and Linux
//   "Inside the project folder"    -> <project>/exports/<kind>  (moves with the project)
// An unsaved project has no folder yet: it falls back to the CAW user folder.
std::string cawExportDir(const char* kind) {
    std::error_code ec;
    std::filesystem::path base;
    if (g_renderInProjectFolder && !g_projectPath.empty()) {
        base = std::filesystem::u8path(g_projectPath).parent_path();
    } else {
        const char* homeEnv =
#if defined(_WIN32)
            std::getenv("USERPROFILE");
#else
            std::getenv("HOME");
#endif
        base = (homeEnv && *homeEnv) ? std::filesystem::u8path(homeEnv) : std::filesystem::temp_directory_path(ec);
        base /= "caw";
    }
    const std::filesystem::path dir = base / "exports" / kind;
    std::filesystem::create_directories(dir, ec);
    return dir.u8string();
}

// The offline render carries the FROZEN tracks' sound through their rendered WAVs (the offline
// host decodes them and feeds them as engine-4 sample-ring inputs - see setOfflineSampleStreams).
// The live sample engine does not run offline, so these streams ARE the locked sources' voices.
std::map<int, CsoundApiRunner::OfflineSampleStream> collectOfflineSampleStreams(const Project& p) {
    std::map<int, CsoundApiRunner::OfflineSampleStream> out;
    for (const Track& tr : p.tracks)
        if (trackFrozen(tr) && !tr.freezePath.empty())
            out[tr.id] = { tr.freezePath, tr.freezeStart, std::max(0.f, std::min(kMaxVolumeGain, tr.volume)) };   // the fader rides on top; the volume lane is baked into the WAV
    return out;
}

// The render-and-lock sequence: queue every UNFROZEN source that must be pre-rendered (Csound /
// sample through their offline passes, SuperCollider real-time), then the rack bounce runs when
// they are done (PD and VSTi sources render natively in the bounce - no freeze needed).
static std::vector<int> g_freezeQueue;      // the source freezes still to run (track indexes)
static int g_freezeVsteBounce = -1;         // the rack to bounce after the queue drains (-1 = none)


// Mixes the finished parts into the export file and hands FLAC/MP3 to the encoders.
static void exportOffMixdown() {
    std::string err;
    const long long total = (long long)std::llround((g_exportOff.t1 - g_exportOff.t0) * (double)g_exportOff.rate);
    std::vector<float> mix((size_t)std::max(1LL, total) * 2, 0.f);
    bool failed = false;
    logLine("[caw] Export: mixing " + std::to_string(g_exportOff.parts.size()) + " part(s) into " + g_exportOff.finalPath + "...");
    for (const ExportOffline::Part& p : g_exportOff.parts) {
        const cawaudio::AudioHandle a = cawaudio::getAudio(p.path, g_exportOff.rate, g_proj.cs.ffmpegExe, err);
        if (!a || a->samples.empty()) { logLine("[caw] Export: a rendered part could not be read (" + p.path + ")" + (err.empty() ? "." : ": " + err)); failed = true; continue; }
        const int ch = std::max(1, a->channels);
        const long long frames = (long long)(a->samples.size() / (size_t)ch);
        long long off = (long long)std::llround((p.t0 - g_exportOff.t0) * (double)g_exportOff.rate);
        long long srcSkip = 0;
        if (off < 0) { srcSkip = -off; off = 0; }   // the part starts before the export range: skip its head
        off = std::max(0LL, std::min(total, off));
        for (long long f = 0; f + srcSkip < frames && off + f < total; ++f)
            for (int c = 0; c < 2; ++c)
                mix[(size_t)((off + f) * 2 + (size_t)c)] += a->samples[(size_t)((f + srcSkip) * (size_t)ch + (size_t)std::min(c, ch - 1))] * p.gain;
    }
    for (const ExportOffline::Part& p : g_exportOff.parts) { std::error_code ec; std::filesystem::remove(std::filesystem::u8path(p.path), ec); }
    g_exportOff.parts.clear();
    if (failed) { g_exportError = "The offline export failed: a part could not be read."; logLine("[caw] " + g_exportError); g_exportOff.active = false; g_renderActive = false; cleanupRenderTemps(); return; }
    CawWavWriter wav;
    if (!wav.open(g_exportOff.tempWav, g_exportOff.rate, 2, g_exportOff.wavFormat, err)) {
        g_exportError = "The export WAV could not be created: " + err;
        logLine("[caw] " + g_exportError);
        g_exportOff.active = false; g_renderActive = false; cleanupRenderTemps();
        return;
    }
    if (g_vstEngineOn) {   // the master FX chain (the FX button): the live mix runs through it before the master gain
        masterfx::Rack& mrack = masterfx::rack();
        bool anyOn = false;
        for (const auto& sv : mrack.view()) if (sv.on && sv.loaded) anyOn = true;
        if (anyOn) {
            int stuck = 0;
            for (long long f = 0; f < total && stuck < 3; f += 512) {
                const int n = (int)std::min<long long>(512, total - f);
                bool done = false;
                for (int attempt = 0; attempt < 100 && !done; ++attempt) {   // render() skips a block when the UI holds the rack: retry
                    done = mrack.render(mix.data() + (size_t)(f * 2), n, 2);
                    if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (!done) ++stuck;
            }
            logLine(stuck < 3 ? "[caw] Export: the master FX chain was applied to the mix."
                              : "[caw] Export: the master FX chain could not process the mix (it stayed busy) - exported without it.");
        }
    }
    const long long chunk = 4800;
    for (long long f = 0; f < total; f += chunk) {
        const long long n = std::min(chunk, total - f);
        if (!wav.write(mix.data() + (size_t)(f * 2), (size_t)n, 1.0, g_proj.cs.masterGain, err)) {
            g_exportError = "The export WAV could not be written (disk problem).";
            logLine("[caw] " + g_exportError);
            g_exportOff.active = false; g_renderActive = false; cleanupRenderTemps();
            return;
        }
    }
    std::string werr;
    if (!wav.finish(werr)) { g_exportError = "The export WAV could not be finished: " + werr; logLine("[caw] " + g_exportError); g_exportOff.active = false; g_renderActive = false; cleanupRenderTemps(); return; }
    logLine("[caw] Offline export mixed " + std::to_string(g_exportOff.partSeq) + " engine part(s) in " +
            std::to_string((int)(std::chrono::duration<double>(std::chrono::steady_clock::now() - g_exportOff.startedAt).count() + 0.5)) + " s.");
    g_exportOff.active = false;
    if (g_exportOff.format >= 3) {
        g_renderConverting = true;
        logLine(g_exportOff.format == 3 ? "[caw] Encoding FLAC..." : "[caw] Encoding MP3...");
        // The bundled ffmpeg first (it is always there); the native Csound encoder only if it is missing.
        if (!startFallbackEncoder() &&
            !g_codecProc.start(g_renderTemp, g_renderEncodedTemp, csoundExePath(), g_exportOff.format, g_activeFlacBits, g_activeMp3Kbps)) {
            g_exportError = "Audio encoder could not start.";
            logLine("[caw] " + g_exportError);
            g_renderActive = false; cleanupRenderTemps();
        }
    } else {
        std::string error;
        std::error_code ec;
        const bool appeared = !g_renderWillOverwrite && std::filesystem::exists(g_renderPath, ec);
        if (ec) { g_exportError = "Could not check the destination: " + ec.message(); logLine("[caw] Audio export failed: " + g_exportError); }
        else if (appeared) { g_exportError = "The destination appeared during rendering; it was not replaced."; logLine("[caw] Audio export failed: " + g_exportError); }
        else if (replaceOutputFile(g_renderTemp, g_renderPath, error)) logLine("[caw] Audio export finished: " + g_renderPath);
        else if (!error.empty()) { g_exportError = error; logLine("[caw] Audio export failed: " + error); }
        g_renderActive = false;
        cleanupRenderTemps();
    }
}

// Starts the next pending track's part (or the mixdown when the queue is empty).
static void exportNextTrack() {
    while (!g_exportOff.pending.empty()) {
        const int idx = g_exportOff.pending.front();
        g_exportOff.pending.erase(g_exportOff.pending.begin());
        if (idx < 0 || idx >= (int)g_proj.tracks.size()) continue;
        const Track& t = g_proj.tracks[(size_t)idx];
        if (t.kind == 1 || t.kind == 2) continue;                          // marker / graph: no sound of their own
        {   // Solo/mute are part of the mix (applied when the queue was built, and again here so a
            // change during the export is respected at the pass level too).
            bool anySolo = false;
            for (const Track& o : g_proj.tracks) if (o.solo && !o.muted) anySolo = true;
            if (t.muted || (anySolo && !t.solo)) continue;
        }
        if (trackFrozen(t)) {                                              // the frozen WAV IS the track's render
            g_exportOff.parts.push_back({t.freezePath, t.freezeStart, std::max(0.f, std::min(kMaxVolumeGain, t.volume))});
            logLine("[caw] Export: \"" + t.name + "\" joins as its frozen WAV.");
            continue;
        }
        if (t.kind == 4) {                                                 // the VSTe rack: its sources' sound, processed
            if (t.vstPath.empty()) { logLine("[caw] Export: the VSTe rack \"" + t.name + "\" has no plugin selected - skipped."); continue; }
            if (t.vstOutMode != 0) { logLine("[caw] Export: the rack \"" + t.name + "\" is in FX+Source mode (the sources also sound on their own) - the offline export would bake them together. Switch it to FX only, or use the real-time mode."); continue; }
            bool pdLib = !g_pdLibraryPath.empty();
            if (!pdLib) g_pdLibraryPath = plat::bundledLibpd();
            pdLib = !g_pdLibraryPath.empty();
            const std::vector<int> sources = freezeVsteSources(idx);
            if (sources.empty()) { logLine("[caw] Export: the rack \"" + t.name + "\" has no renderable sources - skipped."); continue; }
            bool ok = true;
            std::vector<int> purge;                                        // the sources' sound lives INSIDE the bounce
            for (int si : sources) {
                const Track& st = g_proj.tracks[(size_t)si];
                if (trackFrozen(st)) {
                    if (!g_langAudio) { ok = false; logLine("[caw] Export: the rack \"" + t.name + "\" needs Audio enabled (a rendered source feeds it). Skipped."); break; }
                    continue;
                }
                if (st.kind == 3 && !st.vstPath.empty()) { purge.push_back(si); continue; }   // the bounce renders VSTi natively
                if (st.kind == 6 && pdLib) { purge.push_back(si); continue; }                 // the bounce renders PD natively
                ok = false;
                logLine("[caw] Export: the rack \"" + t.name + "\" has a source (\"" + st.name + "\") the offline bounce cannot feed - freeze that source first (LKD) or use the real-time mode. The rack is skipped.");
                break;
            }
            if (!ok) continue;
            for (int si : purge)
                for (size_t k = 0; k < g_exportOff.pending.size(); ++k)
                    if (g_exportOff.pending[(size_t)k] == si) { g_exportOff.pending.erase(g_exportOff.pending.begin() + (long)k); break; }
            g_exportBeginPending = true;
            const bool bounced = freezeVsteBegin(idx);
            g_exportBeginPending = false;
            if (bounced) return;                                           // the bounce runs; the queue continues in phase 2
            logLine("[caw] Export: the rack \"" + t.name + "\" could not render - skipped (see the message above).");
            continue;
        }
        if (t.kind == 3 && t.vstPath.empty()) {
            logLine("[caw] Export: the VSTi track \"" + t.name + "\" has no plugin selected - skipped.");
            continue;
        }
        g_exportBeginPending = true;
        const bool ok = freezeTrackBegin(idx);
        g_exportBeginPending = false;
        if (ok) return;                                                    // the pass runs; freezeTick continues the queue
        logLine("[caw] Export: \"" + t.name + "\" could not render offline - skipped (see the freeze message above).");
    }
    if (g_exportOff.parts.empty()) {
        g_exportError = "The offline export found no renderable tracks.";
        logLine("[caw] " + g_exportError);
        g_exportOff.active = false;
        g_renderActive = false;
        cleanupRenderTemps();
        return;
    }
    exportOffMixdown();
}

// The pure C++ offline renders (rack_offline.h): the rack and PD clips render in CAW's own C++
// hosts - no Csound in those render paths. Polled by freezeTick phase 3, stopped by freezeCancel.
static cawcpp::RackCppRender g_cppRender;

// The pure C++ offline SAMPLE freeze: the sample engine renders the track's clips (their granular
// clip code included) and the MIDI lane's sampler notes, offline - faster than real time, no
// Csound in the render path. While it runs the live streams' render is gated off (setOffline):
// one clock drives the voices. The track's volume automation is sampled into a per-block gain
// curve on the UI thread (the worker never touches the project); the fader stays live afterwards.
static struct SampleCppRender {
    std::atomic<bool> done{false}, stop{false};
    std::atomic<int> code{-1};
    std::mutex logMutex;
    std::vector<std::string> log;
    std::string error;
    std::vector<cawaudio::VoiceSpec> clipVoices;                    // spawned up front (delay = time until the clip starts)
    std::vector<std::pair<double, cawaudio::VoiceSpec>> noteVoices; // spawned on the render clock at their onset
    int trackId = -1, rate = 48000;
    std::string wavPath;
    double seconds = 0.0;
    std::vector<float> gainCurve;   // the track's volume automation, one value per render block

    void say(const std::string& s) { std::lock_guard<std::mutex> l(logMutex); if (log.size() < 256) log.push_back(s); }
    std::vector<std::string> takeLog() { std::lock_guard<std::mutex> l(logMutex); std::vector<std::string> out; log.swap(out); return out; }
    void setError(const std::string& e) { std::lock_guard<std::mutex> l(logMutex); error = e; }
    std::string takeError() { std::lock_guard<std::mutex> l(logMutex); std::string e; error.swap(e); return e; }
    void finish(int c) { code.store(c, std::memory_order_release); done.store(true, std::memory_order_release); }

    void start(std::vector<cawaudio::VoiceSpec> clips,
               std::vector<std::pair<double, cawaudio::VoiceSpec>> notes,
               int trackId_, int rate_, std::string path, double secs, std::vector<float> gains) {
        clipVoices = std::move(clips);
        noteVoices = std::move(notes);
        std::sort(noteVoices.begin(), noteVoices.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        trackId = trackId_; rate = rate_ < 8000 ? 48000 : rate_; wavPath = std::move(path); seconds = secs; gainCurve = std::move(gains);
        done.store(false, std::memory_order_release); stop.store(false, std::memory_order_release); code.store(-1, std::memory_order_release);
        std::thread([this] { run(); }).detach();   // freezeTick polls done/code; freezeCancel sets stop
    }

    void run() {
        auto& eng = cawaudio::engine();
        eng.setSr(rate);
        eng.setOffline(true);   // the live streams stop rendering the sample engine: one clock, no double drive
        eng.clearAll();         // no stale voices from the last play
        auto giveBack = [&]() { eng.setOffline(false); eng.clearAll(); };   // the live voices respawn on the next play
        CawWavWriter wav;
        std::string werr;
        if (!wav.open(wavPath, rate, 2, 2, werr)) { setError(werr); say("[caw] Offline render: " + werr); giveBack(); finish(-1); return; }
        for (const auto& v : clipVoices) eng.startVoice(v);
        size_t noteIdx = 0;
        const int block = 512;
        const double blockDt = (double)block / (double)rate;
        std::vector<float> buf((size_t)block * 2, 0.f);
        unsigned long long frames = 0;
        const unsigned long long cap = seconds > 0.0 ? (unsigned long long)(seconds * (double)rate) : 0;
        while (!stop.load(std::memory_order_relaxed)) {
            if (cap && frames >= cap) break;
            const double tNow = (double)frames / (double)rate;
            if (trackId >= 0) {
                const size_t bi = (size_t)(frames / (unsigned long long)block);
                if (bi < gainCurve.size()) eng.setTrackGain(trackId, gainCurve[bi]);   // the volume lane, live per block
            }
            while (noteIdx < noteVoices.size() && noteVoices[noteIdx].first < tNow + blockDt) {
                cawaudio::VoiceSpec s = noteVoices[noteIdx].second;
                s.t0 = noteVoices[noteIdx].first;   // the render clock IS the project clock: no output latency offline
                s.delay = (long long)std::llround(std::max(0.0, noteVoices[noteIdx].first - tNow) * (double)rate);
                eng.startVoice(s);
                ++noteIdx;
            }
            std::fill(buf.begin(), buf.end(), 0.f);
            eng.render(buf.data(), block, 2, true);   // force: the offline gate is for the live streams
            if (!wav.write(buf.data(), (size_t)block, 1.0, 1.f, werr)) {
                setError(werr); say("[caw] Offline render: " + werr); giveBack(); finish(-1); return;
            }
            frames += (unsigned long long)block;
        }
        giveBack();
        if (stop.load(std::memory_order_relaxed)) {
            wav.close();   // a discarded pass: the sizes stay unwritten
            say("[caw] Offline render: cancelled at " + std::to_string((double)frames / (double)rate) + " s.");
            finish(-1);
            return;
        }
        std::string ferr;
        if (!wav.finish(ferr)) { setError("the render WAV could not be finished: " + ferr); say("[caw] Offline render: " + ferr); finish(-1); return; }
        say("[caw] Offline render: finished normally, " + std::to_string((double)frames / (double)rate) + " s rendered.");
        finish(0);
    }
} g_smpRender;

// The offline SuperCollider freeze: sclang captures every clip's OSC messages in virtual time
// (the embedded CAWNrt class library), then scsynth -N renders the score faster than real time.
// No Csound and no live SC engine in the render path. Polled by freezeTick phase 3 (scOffline).
static scnrt::ScNrtRender g_scRender;

// The SC NRT tools: sclang is required; scsynth.exe must sit next to it (the runtime ships it).
// Non-static: the LKD button pre-gate asks the same question before freezeTrackBegin runs.
bool scNrtAvailable() {
    const std::string sclang = scLangExePath();
    if (sclang.empty()) {
        logLine("[caw] Freeze: no sclang.exe found - the offline SuperCollider render is unavailable.");
        return false;
    }
    std::error_code ec;
    const auto scsynth = std::filesystem::u8path(sclang).parent_path() / (plat::isWindows ? "scsynth.exe" : "scsynth");
    if (!std::filesystem::is_regular_file(scsynth, ec) || ec) {
        logLine("[caw] Freeze: no scsynth.exe next to \"" + sclang + "\" - the offline SuperCollider render is unavailable.");
        return false;
    }
    return true;
}

void freezeWavPaths(const Track& t) {
    const std::filesystem::path dir = cawExportDir("lkd");
    if (g_renderInProjectFolder && g_projectPath.empty())
        logLine("[caw] The project is not saved yet - the render goes to CAW's user folder (save the project to keep renders in project/exports/lkd).");
    std::string safe;
    for (char ch : t.name) {
        const char c = (char)std::tolower((unsigned char)ch);
        safe += ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') ? c : '_';
    }
    if (safe.empty()) safe = "track";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    g_freeze.finalPath = (dir / (safe + "_" + std::to_string(t.id) + ".wav")).u8string();
    g_freeze.tmpPath = (dir / (safe + "_" + std::to_string(t.id) + "_rendering.wav")).u8string();
}

// The writer thread: pops the capture ring on its own (the audio callback never writes to disk).
static void freezeStartWriter() {
    g_freeze.sink.reset();
    g_freeze.stopWriter.store(false, std::memory_order_release);
    FreezeCaptureSink* sink = &g_freeze.sink;
    g_freeze.writer = std::thread([sink]() {
        std::vector<float> block(2048 * 2);
        std::string werr;
        for (;;) {   // the freeze WAV stays stereo: the sink's first two channels
            if (sink->pop(block.data(), 2048, 2)) {
                if (sink == &g_freeze.sink && !g_freeze.wav.write(block.data(), 2048, 1.0, 1.f, werr))
                    g_freeze.writeFailed.store(1, std::memory_order_release);
                continue;
            }
            if (g_freeze.stopWriter.load(std::memory_order_acquire)) {
                while (sink->pop(block.data(), 256, 2))
                    if (sink == &g_freeze.sink && !g_freeze.wav.write(block.data(), 256, 1.0, 1.f, werr))
                        g_freeze.writeFailed.store(1, std::memory_order_release);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    g_freezeSink.store(sink, std::memory_order_release);
    sink->active.store(true, std::memory_order_release);
}

// Starts the freeze render of a track (the lock button). Returns false with a message when the
// track cannot freeze right now (wrong kind, no clips, engines not running, a freeze already runs).
bool freezeTrackBegin(int trackIdx) {
    if (g_freeze.phase != 0) { logLine("[caw] A freeze is already running."); return false; }
    if (trackIdx < 0 || trackIdx >= (int)g_proj.tracks.size()) return false;
    Track& t = g_proj.tracks[(size_t)trackIdx];
    if (!trackCanFreeze(t)) { logLine("[caw] This track kind cannot be frozen (marker, graph and VSTe tracks carry no instrument sound of their own)."); return false; }
    if (trackFrozen(t)) { logLine("[caw] The track is already frozen."); return false; }
    if (g_renderActive && !g_exportBeginPending) { logLine("[caw] Wait for the export to finish before freezing a track."); return false; }
    double lo = 1e300, hi = 0.0;
    for (const Clip& c : t.clips) { lo = std::min(lo, c.start); hi = std::max(hi, c.start + c.dur); }
    for (const MidiClip& mc : t.midiClips) { lo = std::min(lo, mc.start); hi = std::max(hi, mc.start + mc.dur); }
    if (hi <= lo) { logLine("[caw] The track has nothing to render (no clips)."); return false; }

    // The engines must be up only for the REAL-TIME capture kinds (SC): the offline pass renders
    // through its own silent Csound host at full CPU speed - no live engine needed. Sample / VST /
    // PD tracks render offline even with those engines unchecked (the csound DLLs are only the
    // host clock there; PD renders in CAW's C++ PD host mixer); a Csound track itself needs its
    // language enabled, and if the csound DLLs are missing the prepare step falls back to the
    // real-time capture below.
    const bool pdLibAvailable = [&] { if (!g_pdLibraryPath.empty()) return true; g_pdLibraryPath = plat::bundledLibpd(); return !g_pdLibraryPath.empty(); }();
    // Render mode (Settings > Audio): real-time capture skips the offline pipelines entirely -
    // the transport plays once and the callback captures the track's raw mix, for every kind.
    const bool wantOffline = g_proj.cs.renderMode != 1;
    bool offlineCapable = false;
    // g_renderActive normally blocks a freeze (an export owns the offline host) - but an export
    // pass ITSELF (g_exportBeginPending) is the legitimate owner of the offline pipelines.
    const bool renderFree = !g_renderActive || g_exportBeginPending;
    if (wantOffline && renderFree) {
        if (t.kind == 3 || t.kind == 7) offlineCapable = true;
        else if (t.kind == 0) offlineCapable = g_langCsound;
        else if (t.kind == 5) offlineCapable = scNrtAvailable();
        else if (t.kind == 6) offlineCapable = pdLibAvailable;
    }
    bool engineMissing = false;
    const char* what = "";
    if (offlineCapable && t.kind == 3 && t.vstPath.empty()) { logLine("[caw] Freeze: the VSTi track has no plugin selected."); return false; }
    if (!offlineCapable) {   // kind 5 falls back to real time only when the NRT tools are missing; then its SC engine must run
        if (t.kind == 0 && !engineIsUp()) { engineMissing = true; what = "Run Csound (DSP)"; }
        if (t.kind == 0 && engineIsUp() && !g_useCsoundApi) {   // the process-based csound fallback renders outside CAW: nothing to capture
            logLine("[caw] Freeze needs CAW's Csound API engine (the external csound process mode cannot be frozen).");
            return false;
        }
        if (t.kind == 3 && !g_vstEngineOn) { engineMissing = true; what = "Run VST"; }
        if (t.kind == 5 && !g_scReady) { engineMissing = true; what = "Run SuperCollider"; }
        if (t.kind == 6 && !g_pdEngineOn && !g_mixedPd) { engineMissing = true; what = "Run Pure Data (DSP)"; }
        if (t.kind == 7 && !g_sampleEngineOn) { engineMissing = true; what = "Run Sample"; }
        if (engineMissing) { logLine(std::string("[caw] Freeze needs its engine: press ") + what + " first, then freeze the track."); return false; }
    }

    g_freeze.resetForNewPass();
    g_freeze.exportPass = g_exportBeginPending;   // an export-queue part: no lock at the end
    g_freeze.phase = 1;
    g_freeze.trackIdx = trackIdx;
    g_freeze.trackId = t.id;
    g_freeze.trackName = t.name;
    g_freeze.t0 = std::max(0.0, lo);
    g_freeze.t1 = hi + 2.0;                                       // tail: releases and reverb tails ring out
    if (!g_freeze.exportPass) {
        // An LKD replaces the live track: its release tail must survive, like the export's parts do
        // (the fixed 2 s used to cut long plugin decays - the locked track sounded truncated while
        // the export of the same track rang out).
        g_freeze.t1 = hi + std::max(2.0, g_proj.cs.tail + 2.0);
    }
    if (g_freeze.exportPass) {   // the export range (Start/End) bounds this part exactly like the mix
        g_freeze.t0 = std::max(g_freeze.t0, g_exportOff.t0);
        // The part rings to the END OF THE MIX: a plugin's release tail keeps sounding while the
        // transport rolls (the real-time capture hears it), so the offline part must render past
        // its own last clip. The mixdown cuts anything past the export end.
        g_freeze.t1 = std::max(g_freeze.t1, g_exportOff.t1 + 2.0);
        if (g_freeze.t1 <= g_freeze.t0) {
            logLine("[caw] Export: \"" + t.name + "\" lies outside the export range - skipped.");
            return false;
        }
    }
    g_freeze.rate = std::max(8000, g_proj.cs.sr);                 // the device always runs at the project rate

    freezeWavPaths(t);
    std::string err;
    if (!g_freeze.wav.open(g_freeze.tmpPath, g_freeze.rate, 2, 2, err)) {   // float32 stereo
        g_freeze.error = err;
        g_freeze.phase = 0;
        logLine("[caw] Freeze failed: " + err);
        return false;
    }

    // Save every mixer state the pass touches, then set the pass up: only this track is audible,
    // no loop wrap, no master FX, no VSTe routing, the volume slider at unity (it stays live).
    for (const Track& tr : g_proj.tracks) g_freeze.savedMuteSolo.push_back({tr.muted, tr.solo});
    for (const Track& tr : g_proj.tracks) if (tr.kind == 4) {
        std::array<bool, 8> on{};
        for (int s = 0; s < 8; ++s) on[(size_t)s] = tr.vstInputs[s].on;
        g_freeze.savedVstInputs.push_back(on);
    }
    g_freeze.savedLoopEnabled = g_proj.loopEnabled;
    g_freeze.savedLoopStart = g_proj.loopStart;
    g_freeze.savedLoopEnd = g_proj.loopEnd;
    g_freeze.savedMasterChain = g_masterChainEnabled.load(std::memory_order_relaxed);
    g_freeze.savedVolume = t.volume;

    if (timelinePlaying()) toggleTimelinePlay();                  // realtime pass: always plays from t0; offline pass: stay silent
    for (Track& tr : g_proj.tracks) { tr.muted = true; tr.solo = false; }
    t.muted = false;
    t.solo = true;
    t.volume = 1.f;
    for (Track& tr : g_proj.tracks) if (tr.kind == 4) for (int s = 0; s < 8; ++s) tr.vstInputs[s].on = false;
    g_proj.loopEnabled = false;
    g_masterChainEnabled.store(false, std::memory_order_relaxed);
    g_scRawSuppress.store(false, std::memory_order_release);
    g_sampleRawSuppress.store(false, std::memory_order_release);
    pushVstActiveLive();

    if (offlineCapable) {
        // OFFLINE PASS: build the render CSD from the current mixer state (only this track audible),
        // render it through the offline Csound host (silent, full CPU speed) and slice the track's
        // range into the freeze WAV afterwards. PD clips render in CAW's C++ PD host mixer inside
        // the same host (frame-scheduled). Falls back to the real-time capture if the offline host
        // is unavailable.
        std::string error;
        std::vector<pd_timeline::Event> pdPlan;
        // Render ONLY the track that freezes: buildCsd derives the render length from every
        // timeline event of the project, so one stray automation point far right stretches the
        // offline WAV into silent hours until it hits the 4 GiB RIFF limit. A stripped copy keeps
        // the target with its id (the CSD length follows the target's extent instead).
        Project renderProj = g_proj;
        {
            std::vector<Track> keep;
            for (const Track& tr : renderProj.tracks)
                if (tr.id == t.id) keep.push_back(tr);
            renderProj.tracks = std::move(keep);
        }
        // An export pass: mute/solo are already applied (the queue only holds audible tracks), so
        // the render copy must carry them: solo on, mute off - otherwise a copy whose original is
        // unsoloed under an active solo would silently drop its own clip code / lanes.
        if (g_exportBeginPending && !renderProj.tracks.empty())
            renderProj.tracks.front().solo = true, renderProj.tracks.front().muted = false;
        {   // even the target track can carry a stray event far right: clamp the render length to
            // the frozen range (+ tail) so the score stays near the freeze extent
            double endT = 0.0;
            for (const TimelineEvent& ev : timelineEvents(renderProj, 0.0)) endT = std::max(endT, ev.start + ev.dur);
            const double cap = g_freeze.t1 + std::max(0.0, g_proj.cs.tail) + 0.5;
            if (endT > cap) {
                for (Track& tr : renderProj.tracks)
                    for (auto& lane : tr.automation)
                        for (AutoPoint& ap : lane.second)
                            if (ap.t > cap) ap.t = cap;
                logLine("[caw] Freeze: the project timeline stretches to " + std::to_string((int)endT) +
                        " s (a stray event far right) - the offline render is capped at " +
                        std::to_string((int)cap) + " s.");
            }
        }
        g_freeze.csdPath = (workDir() / "caw_freeze.csd").string();
        g_freeze.offlineWavPath = g_freeze.tmpPath + ".full.wav";
        if (t.kind == 7) {
            // PURE C++ SAMPLE FREEZE: the sample engine renders the track's clips (their granular
            // clip code included) and the MIDI lane's sampler notes, offline - no Csound anywhere.
            // The voices carry their project-clock t0 / delay, so one continuous render places
            // every sound exactly where the timeline has it.
            std::vector<cawaudio::VoiceSpec> clipVoices;
            std::vector<std::pair<double, cawaudio::VoiceSpec>> noteVoices;
            // The render runs from PROJECT TIME 0 (the finisher slices [t0, t1] out of the file,
            // assuming it starts at zero - like the Csound / PD offline renders). The voices'
            // delays place every sound at its absolute timeline position.
            const double renderT0 = 0.0;
            for (const Clip& c : t.clips) {
                if (c.muted || c.audioFile.empty() || c.audioSegs.empty() || c.midiEnabled) continue;
                cawaudio::VoiceSpec spec = audioClipVoiceSpec(c, t.id, c.start, t.volume);
                spec.t0 = c.start;   // the render clock IS the project clock: no output latency offline
                spec.delay = (long long)std::llround(std::max(0.0, c.start - renderT0) * (double)g_freeze.rate);
                clipVoices.push_back(std::move(spec));
            }
            for (const MidiClip& mc : t.midiClips) {
                if (mc.muted) continue;
                const Clip* target = findClip(mc.clipId);
                if (!target || target->muted || target->audioFile.empty() || target->audioSegs.empty()) continue;
                std::string derr;
                const cawaudio::AudioHandle data = audioClipData(*target, derr);
                if (!data || data->samples.empty()) continue;
                const double totalSrc = data->duration();
                const double srcStart = target->audioSegs.front().srcOffset;
                double nR = 1.0, nG = 1.0; bool nRev = false, nNorm = false;
                cawaudio::sampleCodeVisuals(target->code, nR, nG, nRev, nNorm);
                for (const MidiNote& note : mc.notes) {
                    const double onset = mc.start + note.start;
                    if (onset < g_freeze.t0 - 1e-9 || onset >= g_freeze.t1) continue;   // sounds before the frozen range are not baked (the live join skips them too)
                    cawaudio::VoiceSpec spec;
                    spec.kind = 1;
                    spec.trackId = t.id;
                    spec.clipId = target->id;
                    spec.t0 = onset;
                    const double pitchRatio = std::pow(2.0, (double)(note.pitch - 60) / 12.0);
                    spec.rate = (1.0 / std::max(0.05, (double)target->audioRate)) * pitchRatio;
                    spec.reverse = target->audioReverse;
                    spec.normalize = nNorm;
                    spec.gain0 = target->audioGain * (double)note.velocity / 100.0 * (double)std::max(0.f, std::min(kMaxVolumeGain, t.volume));
                    double srcPos = srcStart;
                    {
                        double lt = onset - target->start;
                        if (lt < 0.0) lt = 0.0;
                        const double speed = 1.0 / std::max(0.05, (double)target->audioRate);
                        const double want = lt * speed;
                        if (target->audioReverse) {
                            double rem = want;
                            srcPos = srcStart;
                            for (auto it = target->audioSegs.rbegin(); it != target->audioSegs.rend(); ++it) {
                                if (rem < it->dur) { srcPos = it->srcOffset + it->dur - rem; break; }
                                rem -= it->dur;
                                if (std::next(it) == target->audioSegs.rend()) srcPos = it->srcOffset;
                            }
                        } else {
                            double rem = want;
                            srcPos = srcStart;
                            for (const AudioSeg& s : target->audioSegs) {
                                if (rem < s.dur) { srcPos = s.srcOffset + rem; break; }
                                rem -= s.dur;
                                if (&s == &target->audioSegs.back()) srcPos = s.srcOffset + s.dur;
                            }
                        }
                    }
                    spec.srcOffset = srcPos;
                    const double avail = spec.reverse ? srcPos : std::max(0.0, totalSrc - srcPos);
                    spec.noteDur = std::max(0.02, std::min((double)note.dur, std::max(0.02, avail / spec.rate)));
                    if (spec.noteDur <= 0.02) continue;
                    spec.data = data;
                    spec.outSrc = audioClipOutMap(*target, data);
                    spec.script = sampleScript(*target);
                    noteVoices.push_back({onset, std::move(spec)});
                }
            }
            const double renderEnd = g_freeze.t1 + std::max(0.0, g_proj.cs.tail) + 1.0;
            std::vector<float> gainCurve;   // the track's volume automation, one value per render block
            const int smpBlock = 512;
            for (double tt = renderT0; tt < renderEnd; tt += (double)smpBlock / (double)g_freeze.rate)
                gainCurve.push_back(trackVolumeAutomation(t, tt));
            g_freeze.csdPath.clear();
            g_freeze.offlineWavPath = g_freeze.tmpPath + ".full.wav";
            g_smpRender.start(std::move(clipVoices), std::move(noteVoices), t.id, g_freeze.rate,
                              g_freeze.offlineWavPath, renderEnd - renderT0, std::move(gainCurve));
            g_freeze.offline = true;
            g_freeze.smpOffline = true;
            g_freezeSilent.store(false, std::memory_order_release);
            g_freeze.phase = 3;
            g_freeze.startedAt = std::chrono::steady_clock::now();
            logLine("[caw] Freezing track \"" + t.name + "\": OFFLINE render of " +
                    std::to_string(g_freeze.t1 - g_freeze.t0) + " s in CAW's C++ sample engine (clip code and notes included, no Csound) to " + g_freeze.finalPath);
            return true;
        }
        if (t.kind == 5) {
            // PURE SUPERCOLLIDER NRT FREEZE: sclang captures every clip's OSC messages in
            // virtual time (the embedded CAWNrt class library - no live server, no OSC leaves
            // the capture), then scsynth -N renders the score faster than real time into the
            // intermediate WAV (project time 0 to the render end). No Csound in the render path.
            // The clip files carry the same code rewrites the live dispatch applies.
            static const std::regex scPlayTarget(R"re(\.play\s*\(\s*target\s*:\s*~caw[Gg]roup)re");
            static const std::regex scDirectOut(R"re(\bOut\.ar\(\s*0\s*,)re");
            const double renderEnd = g_freeze.t1 + std::max(0.0, g_proj.cs.tail) + 1.0;
            std::string error;
            std::vector<scnrt::ScStart> starts;
            for (const Clip& c : t.clips) {
                if (c.muted || c.code.empty()) continue;
                std::string routed = c.code;
                auto snap = g_scTimelineCode.find(c.id);   // the snapshot the live play uses wins
                if (snap != g_scTimelineCode.end() && !snap->second.empty()) routed = snap->second;
                routed = std::regex_replace(routed, scPlayTarget, ".play(outbus: ~cawoutbus, target: ~cawgroup");
                routed = std::regex_replace(routed, scDirectOut, "Out.ar(~cawoutbus,");
                const std::string path = (workDir() / ("caw_sc_clip_" + std::to_string(c.id) + ".scd")).generic_u8string();
                if (!writeTextFile(path, routed)) { error = "could not write the clip file " + path; break; }
                scnrt::ScStart s;
                s.delay = std::max(0.0, c.start);   // absolute placement: the render runs from project time 0
                s.dur = std::max(0.05, c.dur);      // like a window entry exactly at the clip start
                s.p4 = c.midiEnabled ? 60.0 : 0.0;
                s.p5 = c.midiEnabled ? 100.0 : 0.0;
                s.id = newScInstanceId();
                s.clipId = c.id;
                s.slot = -1;                        // the NRT dispatch forces outBus 0 anyway
                s.path = path;
                starts.push_back(std::move(s));
            }
            // The track's automation lanes: ~cawget reads them live through the hub, so the capture
            // bakes them in - sampled in C++ (autoValueAt, the range/initial like pollScAutomation)
            // and scheduled into virtual time as bus-value updates at 200 Hz (5 ms steps, only the
            // changes) - finer than the live engine's per-block stream, so no staircase zipper.
            std::vector<scnrt::ScLane> lanes;
            int laneSteps = 0;
            for (const auto& lane : t.automation) {
                const std::string& chn = lane.first;
                const std::vector<AutoPoint>& pts = lane.second;
                if (pts.empty() || !autoLaneActive(t, chn)) continue;
                double lo = 0.0, hi = 1.0, initial = 0.5;
                const auto range = t.automationRanges.find(chn);
                if (range != t.automationRanges.end()) {
                    lo = range->second.min;
                    hi = range->second.max > lo ? range->second.max : lo + 1.0;
                    initial = range->second.value;
                }
                scnrt::ScLane L;
                L.name = chn;
                for (double tt = 0.0; tt < renderEnd; tt += 0.005) {
                    const double v = autoValueAt(pts, tt, initial);
                    if (L.steps.empty() || std::fabs(v - L.steps.back().second) > 1e-4) L.steps.push_back({tt, v});
                }
                laneSteps += (int)L.steps.size();
                if (!L.steps.empty()) lanes.push_back(std::move(L));
            }
            if (error.empty() && starts.empty()) error = "the track has no SuperCollider clip code to render.";
            if (error.empty()) {
                std::string libErr;
                if (!scnrt::installClassLibrary(scLangExePath(), libErr)) error = libErr;
            }
            std::string inWavPath, scriptPath, scorePath;
            if (error.empty()) {
                // scsynth -N requires a real input file: render end seconds of silence at the project rate.
                inWavPath = g_freeze.tmpPath + ".scin.wav";
                CawWavWriter inwav;
                std::string werr;
                if (!inwav.open(inWavPath, g_freeze.rate, 2, 2, werr)) {
                    error = "could not write the NRT input WAV: " + werr;
                } else {
                    std::vector<float> zeros(1024, 0.f);
                    const long long frames = (long long)std::llround(g_freeze.t1 + std::max(0.0, g_proj.cs.tail) + 1.0);
                    for (long long f = 0; f < frames && werr.empty(); f += 512)
                        inwav.write(zeros.data(), 512, 1.0, 1.f, werr);
                    if (werr.empty() && !inwav.finish(werr)) werr = werr.empty() ? "could not finish" : werr;
                    if (!werr.empty()) error = "could not write the NRT input WAV: " + werr;
                }
            }
            if (error.empty()) {
                scriptPath = (workDir() / "caw_sc_nrt_capture.scd").generic_u8string();
                scorePath = g_freeze.tmpPath + ".osc";
                if (!writeTextFile(scriptPath, scnrt::captureScript(starts, lanes, scorePath, renderEnd)))
                    error = "could not write the NRT capture script.";
            }
            if (error.empty()) {
                const auto sclangDir = std::filesystem::u8path(scLangExePath()).parent_path();
                const auto scsynthExe = sclangDir / (plat::isWindows ? "scsynth.exe" : "scsynth");
                const std::string conf = writePrivateSclangConf();
                const std::string sclangCmd = processArg(scLangExePath()) + (conf.empty() ? "" : " -l " + processArg(conf)) +
                                              " " + processArg(scriptPath);
                // The scsynth output IS the intermediate file the finisher slices - set the path
                // BEFORE the command line is built, and keep it for the finisher.
                g_freeze.csdPath.clear();
                g_freeze.offlineWavPath = g_freeze.tmpPath + ".full.wav";
                const std::string scsynthCmd = processArg(scsynthExe.u8string()) + " -N " + processArg(scorePath) + " " +
                                               processArg(inWavPath) + " " + processArg(g_freeze.offlineWavPath) +
                                               " " + std::to_string(g_freeze.rate) + " wav int24 -o 2";
                g_scRender.start(t.name, sclangCmd, scsynthCmd, scorePath, g_freeze.offlineWavPath,
                                 std::max(180.0, renderEnd + 60.0), std::max(120.0, renderEnd + 60.0));
                g_freeze.offline = true;
                g_freeze.scOffline = true;
                g_freezeSilent.store(false, std::memory_order_release);
                g_freeze.phase = 3;
                g_freeze.startedAt = std::chrono::steady_clock::now();
                logLine("[caw] Freezing track \"" + t.name + "\": OFFLINE render of " +
                        std::to_string(g_freeze.t1 - g_freeze.t0) + " s with the SuperCollider NRT renderer (sclang virtual-time capture + scsynth -N, no Csound) to " + g_freeze.finalPath);
                logLine("[caw] NRT tools: sclang = " + scLangExePath() + ", scsynth = " + scsynthExe.u8string());
                logLine("[caw] NRT clips queued: " + std::to_string(starts.size()) + " (capture script " + scriptPath + ")");
                return true;
            }
            logLine("[caw] Offline SC freeze not available (" + error + ") - falling back to the real-time capture.");
        }
        if (t.kind == 6) {
            // PURE C++ PD FREEZE: the C++ PD host mixer renders the clips directly into the
            // intermediate WAV - no Csound anywhere in the render path (the user asked for PD
            // offline in C++; the Csound-hosted PD render is gone).
            int bad = 0; std::string pdErr;
            std::vector<pd_timeline::Event> pdPlan;
            if (!pd_timeline::makePlan(renderProj, g_projectPath, pdPlan, pdErr, false, &bad, true)) {
                error = "PD clip problem: " + pdErr;
            } else {
                g_freeze.csdPath.clear();
                g_cppRender.startPd({}, {}, {}, g_freeze.rate, g_freeze.offlineWavPath,
                                    g_freeze.t1 + std::max(0.0, g_proj.cs.tail) + 1.0, g_pdLibraryPath, std::move(pdPlan));
                g_freeze.offline = true;
                g_freeze.cppOffline = true;
                g_freezeSilent.store(false, std::memory_order_release);
                g_freeze.phase = 3;
                g_freeze.startedAt = std::chrono::steady_clock::now();
                logLine("[caw] Freezing track \"" + t.name + "\": OFFLINE render of " +
                        std::to_string(g_freeze.t1 - g_freeze.t0) + " s in CAW's C++ PD host (no Csound in the render path) to " + g_freeze.finalPath);
                return true;
            }
        }
        if (t.kind == 3 && t.vstPath.empty()) error = "the VSTi track has no plugin selected.";
        if (t.kind == 3 && error.empty()) {
            // PURE C++ VSTi FREEZE: the plugin renders in CAW's C++ VST host - MIDI clips, clip
            // code (notes and P1..P8) and drawn automation lanes included; no Csound in the path.
            vstui::captureAllParameters(g_proj);   // the render uses exactly what the plugin GUIs show
            std::vector<VstJob> jobs = collectVstJobs(renderProj, {});
            captureLiveVstInstanceState(jobs);   // the offline plugin starts where the LIVE sound is
            std::vector<cawcpp::RackCppRender::Note> notes;
            for (const VstSoloNote& sn : buildVstSoloNotes())
                if (sn.trackId == t.id) notes.push_back({sn.start, sn.dur, t.id, sn.pitch, sn.velocity});
            for (const VstJob& j : jobs) {   // parameter evidence: what the offline plugin will start from
                std::string picks;
                for (int k = 0; k < 8 && k < (int)j.pick.size(); ++k)
                    picks += " P" + std::to_string(k + 1) + "->param" + std::to_string(j.pick[k]) +
                             (k < (int)j.init.size() ? "(start " + std::to_string(j.init[(size_t)k]).substr(0, 5) + ")" : "");
                logLine("[caw] Export part \"" + t.name + "\": plugin params saved=" + std::to_string(j.parameterState.size()) +
                        ", state blob=" + std::to_string(j.stateBlob.size()) + " bytes," + (picks.empty() ? " no picked slots" : picks));
            }
            g_freeze.csdPath.clear();
            g_freeze.offlineWavPath = g_freeze.tmpPath + ".full.wav";
            const double vstiEnd = g_freeze.t1 + std::max(0.0, g_proj.cs.tail) + 1.0;
            g_cppRender.start(std::move(jobs), {}, std::move(notes), g_freeze.rate, g_freeze.offlineWavPath,
                              vstiEnd, buildVstScripts(renderProj));
            g_freeze.offline = true;
            g_freeze.cppOffline = true;
            g_freezeSilent.store(false, std::memory_order_release);
            g_freeze.phase = 3;
            g_freeze.startedAt = std::chrono::steady_clock::now();
            logLine("[caw] Freezing track \"" + t.name + "\": OFFLINE render of " +
                    std::to_string(g_freeze.t1 - g_freeze.t0) + " s in CAW's C++ VST host (clip code, notes and "
                    "automation included, no Csound) to " + g_freeze.finalPath);
            return true;
        }
        if (error.empty() && !writeTextFile(g_freeze.csdPath, buildCsd(renderProj, "render", 0.0, &g_orcSpans))) {
            error = "could not write the temporary render CSD.";
        } else if (error.empty()) {
            shareEngineDlls();
            if (!g_offlineProc.prepare(csoundExePath(), error)) error = "the offline renderer is unavailable: " + error;
        }
        if (error.empty()) {
            vstui::captureAllParameters(g_proj);   // the render uses exactly what the plugin GUIs show
            g_offlineProc.setPdSource(t.kind == 6 ? g_pdLibraryPath : std::string(), t.kind == 6 ? pdPlan : std::vector<pd_timeline::Event>{});
            g_offlineProc.setVstJobs(collectVstJobs(renderProj, {}));
            g_offlineProc.setControlChannels(vstGateValues());
            g_offlineProc.setOfflineSampleStreams(collectOfflineSampleStreams(renderProj));   // a locked source in the render copy feeds through its WAV
            const int partCh = (g_exportBeginPending && g_exportOff.active) ? std::min(64, std::max(1, g_exportOff.partCh)) : 2;
            if (!g_offlineProc.startOffline(g_freeze.csdPath, g_freeze.rate, masterChannels(g_proj.cs),
                                            (float)g_proj.cs.zdbfs, 1.f, 2, g_freeze.offlineWavPath, error, partCh,
                                            g_freeze.t1 + std::max(0.0, g_proj.cs.tail) + 1.0))
                error = "the offline render could not start: " + error;
            else if (t.kind == 6)
                g_offlineProc.pdTransport(true, g_freeze.t0, 0.0, 0.0, false, g_freeze.rate, 1);   // the PD mixer rolls from the freeze start (no ack handshake offline)
        }
        if (error.empty()) {
            g_freeze.offline = true;
            g_freezeSilent.store(false, std::memory_order_release);
            g_freeze.phase = 3;
            g_freeze.startedAt = std::chrono::steady_clock::now();
            logLine("[caw] Freezing track \"" + t.name + "\": OFFLINE render of " +
                    std::to_string(g_freeze.t1 - g_freeze.t0) + " s (silent, faster than real time) to " + g_freeze.finalPath);
            return true;
        }
        logLine("[caw] Offline freeze not available (" + error + ") - falling back to the real-time capture.");
        g_freeze.csdPath.clear();
        g_freeze.offlineWavPath.clear();
        // the real-time capture needs the live engines even for kinds the offline pass did not
        engineMissing = false;
        if (t.kind == 0 && !engineIsUp()) { engineMissing = true; what = "Run Csound (DSP)"; }
        else if (t.kind == 0 && !g_useCsoundApi) { logLine("[caw] Freeze needs CAW's Csound API engine (the external csound process mode cannot be frozen)."); return false; }
        else if (t.kind == 3 && !g_vstEngineOn) { engineMissing = true; what = "Run VST"; }
        else if (t.kind == 6 && !g_pdEngineOn && !g_mixedPd) { engineMissing = true; what = "Run Pure Data (DSP)"; }
        else if (t.kind == 7 && !g_sampleEngineOn) { engineMissing = true; what = "Run Sample"; }
        if (engineMissing) { logLine(std::string("[caw] Freeze needs its engine: press ") + what + " first, then freeze the track."); return false; }
    }

    // The writer thread takes it from here; the transport rolls and freezeTick watches the playhead.
    g_freezeSilent.store(true, std::memory_order_release);   // the capture keeps working; the speakers stay silent
    freezeStartWriter();
    seekTimeline(g_freeze.t0);
    toggleTimelinePlay();
    logLine("[caw] Freezing track \"" + t.name + "\": rendering " +
            std::to_string(g_freeze.t1 - g_freeze.t0) + " s to " + g_freeze.finalPath);
    return true;
}

// Aborts the pass (Cancel button or an error): restore everything, drop the partial WAV.
// Called every UI frame. Watches the playhead; when it passes the end of the extent (or the
// transport died) the pass finalizes: stop, drain, close the WAV, restore the mixer, freeze.
// The pure C++ offline renders (g_cppRender) poll through the same phase 3.
void freezeCancel() {
    if (g_freeze.phase == 0) return;
    if (g_freeze.phase == 3) {
        if (g_freeze.cppOffline) g_cppRender.stop.store(true, std::memory_order_release);
        else if (g_freeze.smpOffline) g_smpRender.stop.store(true, std::memory_order_release);
        else if (g_freeze.scOffline) g_scRender.stop.store(true, std::memory_order_release);
        else {
            g_offlineProc.stop();
            for (const std::string& l : g_offlineProc.takeOutput()) logLine(l);
        }
    }
    if (timelinePlaying()) toggleTimelinePlay();
    freezeStopWriter(false);
    freezeRestoreStates();
    std::error_code ec;
    std::filesystem::remove(std::filesystem::u8path(g_freeze.tmpPath), ec);
    if (g_freeze.phase == 3) {
        if (!g_freeze.offlineWavPath.empty()) std::filesystem::remove(std::filesystem::u8path(g_freeze.offlineWavPath), ec);
        if (!g_freeze.csdPath.empty()) std::filesystem::remove(std::filesystem::u8path(g_freeze.csdPath), ec);
        // the SC NRT intermediates: the OSC score and the silent input WAV (only exist for SC passes)
        std::filesystem::remove(std::filesystem::u8path(g_freeze.tmpPath + ".osc"), ec);
        std::filesystem::remove(std::filesystem::u8path(g_freeze.tmpPath + ".scin.wav"), ec);
        g_freeze.offline = false;
        g_freeze.cppOffline = false;
        g_freeze.smpOffline = false;
        g_freeze.scOffline = false;
    }
    logLine("[caw] Freeze cancelled: nothing was changed.");
    g_freezeQueue.clear();
    g_freezeVsteBounce = -1;
    g_freeze.phase = 0;
}

// Called every UI frame. Watches the playhead; when it passes the end of the extent (or the
// transport died) the pass finalizes: stop, drain, close the WAV, restore the mixer, freeze.
// The pure C++ offline rack bounce (g_cppRender) polls through the same phase 3.
void freezeTick() {
    if (g_freeze.phase == 0) return;
    if (g_freeze.phase == 3) {   // OFFLINE pass: poll the render, then slice the track's range into the freeze WAV
        if (g_freeze.smpOffline) {
            if (!g_smpRender.done.load(std::memory_order_acquire)) return;
            for (const std::string& l : g_smpRender.takeLog()) logLine(l);
            const int code = g_smpRender.code.load(std::memory_order_acquire);
            if (code != 0) {
                const std::string why = g_smpRender.takeError();
                g_freeze.error = "The offline render failed" + (why.empty() ? std::string(".") : ": " + why);
            }
        } else if (g_freeze.cppOffline) {
            if (!g_cppRender.done.load(std::memory_order_acquire)) return;
            for (const std::string& l : g_cppRender.takeLog()) logLine(l);
            const int code = g_cppRender.code.load(std::memory_order_acquire);
            if (code != 0) {
                const std::string why = g_cppRender.takeError();
                g_freeze.error = "The offline render failed" + (why.empty() ? std::string(".") : ": " + why);
            }
        } else if (g_freeze.scOffline) {
            if (!g_scRender.done.load(std::memory_order_acquire)) return;
            for (const std::string& l : g_scRender.takeLog()) logLine(l);
            const int code = g_scRender.code.load(std::memory_order_acquire);
            if (code != 0) {
                const std::string why = g_scRender.takeError();
                g_freeze.error = "The offline render failed" + (why.empty() ? std::string(".") : ": " + why);
            }
        } else {
        if (g_offlineProc.running()) return;
        for (const std::string& l : g_offlineProc.takeOutput()) logLine(l);
        const int code = g_offlineProc.exitCode();
        if (!g_offlineProc.finished() && code == 0) {
            // The worker stores live=false before done=true: one frame can see neither running
            // nor finished. Wait a few frames before declaring a bogus failure (exit code 0).
            if (++g_freeze.settle < 20) return;
            g_freeze.error = "The offline render did not report completion.";
        } else if (code != 0) {
            g_freeze.error = "The offline render failed (exit code " + std::to_string(code) + ").";
        }
        }
        if (!g_freeze.error.empty()) {
            logLine("[caw] " + g_freeze.error);
        } else {
            std::string err;
            const cawaudio::AudioHandle mix = cawaudio::getAudio(g_freeze.offlineWavPath, g_freeze.rate, g_proj.cs.ffmpegExe, err);
            if (!mix || mix->samples.empty()) {
                g_freeze.error = "The offline render produced no audio" + std::string(err.empty() ? "." : ": " + err);
                logLine("[caw] " + g_freeze.error);
            } else {
                const int ch = std::max(1, mix->channels);
                const int mixSr = std::max(8000, mix->sr);
                const long long total = (long long)(mix->samples.size() / (size_t)ch);
                long long f0 = (long long)std::llround(g_freeze.t0 * (double)mixSr);
                long long f1 = (long long)std::llround(g_freeze.t1 * (double)mixSr);
                f0 = std::max(0LL, std::min(total, f0));
                f1 = std::max(f0, std::min(total, f1));   // the tail beyond the project end is not in the render
                if (f1 - f0 < 16) {
                    g_freeze.error = "The rendered range is empty.";
                    logLine("[caw] " + g_freeze.error);
                } else {
                    // write stereo float32 into the freeze WAV (mono renders duplicate, >2 ch take the first two)
                    std::string werr;
                    std::vector<float> blk;
                    const long long chunk = 4800;
                    for (long long f = f0; f < f1 && g_freeze.error.empty(); f += chunk) {
                        const long long n = std::min(chunk, f1 - f);
                        blk.clear();
                        blk.reserve((size_t)n * 2u);
                        for (long long k = 0; k < n; ++k)
                            for (int c = 0; c < 2; ++c)
                                blk.push_back(mix->samples[(size_t)((f + k) * (size_t)ch + (size_t)std::min(c, ch - 1))]);
                        if (!g_freeze.wav.write(blk.data(), (size_t)n, 1.0, 1.f, werr)) {
                            g_freeze.error = "The freeze WAV could not be written (disk problem).";
                            g_freeze.writeFailed.store(1, std::memory_order_release);
                            logLine("[caw] " + g_freeze.error + " (" + werr + ")");
                        }
                    }
                }
            }
            std::error_code ec;
            if (!g_freeze.offlineWavPath.empty()) std::filesystem::remove(std::filesystem::u8path(g_freeze.offlineWavPath), ec);
            if (!g_freeze.csdPath.empty()) std::filesystem::remove(std::filesystem::u8path(g_freeze.csdPath), ec);
            // the SC NRT intermediates: the OSC score and the silent input WAV
            std::filesystem::remove(std::filesystem::u8path(g_freeze.tmpPath + ".osc"), ec);
            std::filesystem::remove(std::filesystem::u8path(g_freeze.tmpPath + ".scin.wav"), ec);
            g_freeze.offline = false;
            g_freeze.cppOffline = false;
            g_freeze.smpOffline = false;
            g_freeze.scOffline = false;
        }
        if (g_freeze.error.empty())
            logLine("[caw] Offline render finished in " + std::to_string((int)(std::chrono::duration<double>(std::chrono::steady_clock::now() - g_freeze.startedAt).count() + 0.5)) + " s. Locking the track...");
        g_freeze.phase = 2;   // the phase-2 finalizer stops the writer, restores the mixer and locks the track
        return;
    }
    if (g_freeze.phase == 1) {
        if (!timelinePlaying()) {                                 // Play could not start (PD problem etc.)
            g_freeze.error = "The transport did not start - the freeze was cancelled.";
            logLine("[caw] " + g_freeze.error);
            freezeCancel();
            return;
        }
        if (g_play < g_freeze.t0 - 0.5) {                         // the playhead was moved away mid-pass
            g_freeze.error = "The playhead left the rendered range - the freeze was cancelled.";
            logLine("[caw] " + g_freeze.error);
            freezeCancel();
            return;
        }
        if (g_freeze.writeFailed.load(std::memory_order_acquire)) {   // disk problem while rendering
            g_freeze.error = "The freeze WAV could not be written (disk problem).";
            logLine("[caw] " + g_freeze.error);
            freezeCancel();
            return;
        }
        if (g_play >= g_freeze.t1 - 1e-6) {                       // reached the end of the extent
            g_freeze.phase = 2;
        }
        return;
    }
    // phase 2: the transport is stopped, the last blocks drain from the ring into the WAV.
    if (timelinePlaying()) toggleTimelinePlay();
    freezeStopWriter(true);
    freezeRestoreStates();
    if (g_freeze.writeFailed.load(std::memory_order_acquire) && g_freeze.error.empty())
        g_freeze.error = "The freeze WAV could not be written (disk problem).";
    Track& t = g_proj.tracks[(size_t)g_freeze.trackIdx];
    if (g_freeze.exportPass) {   // whole-project offline export: this pass made ONE part - no lock
        if (!g_freeze.error.empty()) {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::u8path(g_freeze.tmpPath), ec);
            logLine("[caw] Export part failed for \"" + g_freeze.trackName + "\": " + g_freeze.error);
        } else {
            const std::string partPath = (workDir() / ("caw_export_part_" + std::to_string(g_exportOff.partSeq++) + ".wav")).generic_u8string();
            std::error_code ec;
            std::filesystem::rename(std::filesystem::u8path(g_freeze.tmpPath), std::filesystem::u8path(partPath), ec);
            if (ec) logLine("[caw] Export: the part WAV could not be kept: " + ec.message());
            else {
                g_exportOff.parts.push_back({partPath, g_freeze.t0, std::max(0.f, std::min(kMaxVolumeGain, t.volume))});   // t.volume is restored by now: the freeze pass rendered at unity
                logLine("[caw] Export: \"" + g_freeze.trackName + "\" rendered (part " + std::to_string(g_exportOff.partSeq) + ").");
            }
        }
        g_freeze.phase = 0;
        if (g_exportOff.active) exportNextTrack();
        return;
    }
    if (g_freeze.error.empty()) {
        std::error_code ec;
        std::filesystem::rename(std::filesystem::u8path(g_freeze.tmpPath), std::filesystem::u8path(g_freeze.finalPath), ec);
        if (ec) {
            g_freeze.error = "Could not move the rendered WAV into place: " + ec.message();
        } else {
            cawUndoDropBackup(g_freeze.finalPath);   // a fresh render supersedes any backup of this location
            t.frozen = true;
            t.freezePath = g_freeze.finalPath;
            t.freezeStart = g_freeze.t0;
            t.freezeEnd = g_freeze.t1;
            for (Clip& c : t.clips) c.editorOpen = false;         // the frozen track is locked: its editors close
            g_audioVoiceSig.clear();                              // the sample voices rebuild: the WAV voice takes over
            if (!g_sampleEngineOn && g_langAudio) {               // the frozen WAV plays through the sample engine: start it so the result is audible
                g_sampleEngineOn = true;
                ensureSampleOutput();
                logLine("[caw] Sample engine started automatically: frozen tracks play their rendered WAV through it (the track fader stays live on it).");
            }
            if (g_freeze.vsteBounce) {                            // the rack is locked: its sources go silent (the WAV now carries their processed sound)
                for (int si : freezeVsteSources(g_freeze.trackIdx)) {
                    Track& so = g_proj.tracks[(size_t)si];
                    so.muted = true;
                    so.solo = false;
                }
            }
            pushVstActiveLive();
            logLine("[caw] Track \"" + t.name + "\" frozen: " +
                    std::to_string(g_freeze.t1 - g_freeze.t0) + " s rendered. The engines skip it until you unfreeze it.");
        }
    }
    if (!g_freeze.error.empty()) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::u8path(g_freeze.tmpPath), ec);
        logLine("[caw] Freeze failed: " + g_freeze.error);
        g_freeze.phase = 0;
        return;
    }
    g_freeze.phase = 0;
    // The render-and-lock sequence: the next source starts right away, and the rack bounce runs
    // when the queue drains.
    if (!g_freezeQueue.empty()) {
        const int next = g_freezeQueue.front();
        g_freezeQueue.erase(g_freezeQueue.begin());
        if (!freezeTrackBegin(next)) { g_freezeQueue.clear(); g_freezeVsteBounce = -1; }
        return;
    }
    if (g_freezeVsteBounce >= 0) {
        const int rack = g_freezeVsteBounce;
        g_freezeVsteBounce = -1;
        if (!freezeVsteBegin(rack)) g_freezeQueue.clear();
        return;
    }
}

// Unfreezes (unlock): the WAV is deleted, the live engines play the track again.
// A locked VSTe rack also muted its source tracks (their processed sound lives in the WAV):
// unlocking brings them back live.
void freezeUnfreeze(int trackIdx) {
    if (trackIdx < 0 || trackIdx >= (int)g_proj.tracks.size()) return;
    Track& t = g_proj.tracks[(size_t)trackIdx];
    if (!trackFrozen(t)) { t.frozen = false; t.freezePath.clear(); t.freezeStart = t.freezeEnd = 0.0; return; }
    std::error_code ec;
    cawUndoKeepFile(t.freezePath);   // not deleted: undo may need to bring this WAV back
    t.frozen = false;
    t.freezePath.clear();
    t.freezeStart = t.freezeEnd = 0.0;
    if (t.kind == 4) {
        for (Track& o : g_proj.tracks) {
            if (o.kind == 4) continue;
            bool isSource = false;
            for (int s = 0; s < 8 && !isSource; ++s) {
                const auto& in = t.vstInputs[s];
                if (!in.on || in.mode == 2) continue;
                isSource = in.mode == 0 ? o.name == in.name
                    : std::any_of(o.clips.begin(), o.clips.end(), [&](const Clip& c) { return c.name == in.name; });
            }
            if (isSource) { o.muted = false; o.solo = false; }
        }
        logLine("[caw] Unlocked the rack \"" + t.name + "\": its source tracks play live again.");
    } else {
        logLine("[caw] Unfroze track \"" + t.name + "\": the live engines play it again.");
    }
    cawaudio::engine().killManaged(freezeClipId(t.id));           // the frozen WAV voice stops right now
    g_audioVoiceSig.clear();                                      // and the sample voices rebuild on the next tick
    clearSampleRings();
    if (g_liveCsound && g_useCsoundApi) g_apiProc.silenceVstTaps();
    pushVstActiveLive();
    if (g_timelineRun) g_resync = 1;   // restart clips that already began at the current playhead
}

// --------------------------------------------------------------- VSTe rack: render-and-lock
// The enabled inputs of a rack (mode 0 = track by name, 1 = clip by name; mode 2 is not audio)
// resolve to source tracks. Live sources feed the plugins directly; already-locked sources feed
// their existing WAV voices. Locking the rack never freezes its sources first.
std::vector<int> freezeVsteSources(int rackIdx) {
    std::vector<int> out;
    if (rackIdx < 0 || rackIdx >= (int)g_proj.tracks.size()) return out;
    const Track& rack = g_proj.tracks[(size_t)rackIdx];
    if (rack.kind != 4) return out;
    for (int s = 0; s < 8; ++s) {
        const auto& in = rack.vstInputs[s];
        if (!in.on || in.mode == 2) continue;
        for (size_t i = 0; i < g_proj.tracks.size(); ++i) {
            const Track& o = g_proj.tracks[i];
            if ((int)i == rackIdx) continue;
            if (o.kind != 0 && o.kind != 3 && o.kind != 5 && o.kind != 6 && o.kind != 7) continue;
            const bool match = in.mode == 0 ? o.name == in.name
                : std::any_of(o.clips.begin(), o.clips.end(), [&](const Clip& c) { return c.name == in.name; });
            if (!match) continue;
            if (std::find(out.begin(), out.end(), (int)i) == out.end()) out.push_back((int)i);
        }
    }
    return out;
}

// Everything the render-and-lock sequence needs, checked without side effects (the modal shows
// `problem` next to a disabled button while the sequence cannot run).
bool freezeVsteReady(int rackIdx, std::string& problem) {
    problem.clear();
    if (rackIdx < 0 || rackIdx >= (int)g_proj.tracks.size()) { problem = "The track is gone."; return false; }
    const Track& rack = g_proj.tracks[(size_t)rackIdx];
    if (rack.kind != 4) { problem = "Not a VSTe track."; return false; }
    if (rack.vstPath.empty()) { problem = "Pick a plugin for this VSTe track first."; return false; }
    if (trackFrozen(rack)) { problem = "This rack is already locked. Press LKD again to unlock it."; return false; }
    if (freezeActive()) { problem = "A freeze is already running - wait for it to finish."; return false; }
    if (g_renderActive) { problem = "An audio export is running - wait for it to finish first."; return false; }
    const std::vector<int> sources = freezeVsteSources(rackIdx);
    if (sources.empty()) { problem = "No renderable sources: the enabled inputs name no Csound / VSTi / SuperCollider / Pure Data / Sample track."; return false; }
    if (rack.vstOutMode != 0) { problem = "This rack is in FX+Source mode: the sources keep sounding next to the effect, so a lock would bake them together. Switch the rack to FX only first."; return false; }
    // Rack locking runs OFFLINE in CAW's own C++ hosts when possible: the sources that must be
    // pre-rendered (Csound through the Csound offline host; sample through its offline pass;
    // SuperCollider still real-time) are queued one by one, and the bounce then runs in the pure
    // C++ VST host with every source locked. VSTi and PD sources render natively in the bounce
    // (no freeze, no Csound). Only a graph that cannot go offline falls back to the real-time
    // capture - and that fallback needs the live engines.
    bool pdLib = [&] { if (!g_pdLibraryPath.empty()) return true; g_pdLibraryPath = plat::bundledLibpd(); return !g_pdLibraryPath.empty(); }();
    bool allNative = !rack.vstPath.empty();
    for (int si : sources) {
        const Track& st = g_proj.tracks[(size_t)si];
        if (trackFrozen(st)) { if (!g_langAudio) { problem = "Enable Audio in Settings > Engines: a rendered source feeds this rack through the sample engine."; return false; } continue; }
        if (st.kind == 3 && !st.vstPath.empty()) continue;      // VSTi: rendered natively in the C++ bounce
        if (st.kind == 6 && pdLib) continue;                    // PD: rendered by the C++ PD host mixer in the bounce
        allNative = false;
    }
    if (allNative) return true;   // the whole sequence runs offline in C++ / the Csound source host: no live engines needed
    if (!g_vstEngineOn) { problem = "Run VST first: the rack's plugins do the processing."; return false; }
    for (int si : sources) {
        const Track& st = g_proj.tracks[(size_t)si];
        if (trackFrozen(st)) {
            if (!g_langAudio) { problem = "Enable Audio in Settings > Engines: a rendered source feeds this rack through the sample engine."; return false; }
            continue;
        }
        if (st.kind == 0 && !g_langCsound) { problem = "Enable Csound in Settings > Engines: a source track uses it (its offline render needs it)."; return false; }
        if (st.kind == 5 && !g_scReady) { problem = "Run SuperCollider first: a source track uses it."; return false; }
        if (st.kind == 6 && !g_pdEngineOn && !g_mixedPd) { problem = "Run Pure Data (DSP) first: a source track uses it."; return false; }
        if (st.kind == 7 && !g_langAudio) { problem = "Enable Audio in Settings > Engines: a live sample source feeds this rack."; return false; }
    }
    return true;
}


bool freezeVsteRenderAll(int rackIdx) {
    std::string problem;
    if (!freezeVsteReady(rackIdx, problem)) { logLine("[caw] " + problem); return false; }
    const std::vector<int> sources = freezeVsteSources(rackIdx);
    std::vector<int> pending;
    for (int si : sources) {
        const Track& st = g_proj.tracks[(size_t)si];
        if (trackFrozen(st)) continue;
        if (st.kind == 3 || st.kind == 6) continue;   // VSTi / PD: the bounce renders them natively
        pending.push_back(si);
    }
    g_freezeQueue = pending;
    g_freezeVsteBounce = rackIdx;
    if (g_freezeQueue.empty()) {   // every source renders natively or is locked: bounce right away
        g_freezeQueue.clear();
        return freezeVsteBegin(rackIdx);
    }
    const int first = g_freezeQueue.front();
    g_freezeQueue.erase(g_freezeQueue.begin());
    if (!freezeTrackBegin(first)) {
        g_freezeQueue.clear();
        g_freezeVsteBounce = -1;
        return false;
    }
    logLine("[caw] Render-and-lock: " + std::to_string(pending.size()) + " source(s) first, then the rack bounce (all offline - the render takes seconds, not the playing time).");
    return true;
}

// The rack bounce: plays the transport once through the sources' extent with ONLY the rack audible
// (its inputs stay wired, so the plugins process their live sources; every other track is muted and
// the replace-mode routing already silences the raw source mixes). The WAV replaces the rack: the
// frozen rack runs no plugin instances (the WAV voice plays instead) and its sources stay muted
// until the rack is unlocked.
bool freezeVsteBegin(int rackIdx) {
    if (g_freeze.phase != 0) { logLine("[caw] A freeze is already running."); return false; }
    if (rackIdx < 0 || rackIdx >= (int)g_proj.tracks.size()) return false;
    Track& rack = g_proj.tracks[(size_t)rackIdx];
    if (rack.kind != 4 || trackFrozen(rack)) return false;
    if (g_renderActive && !g_exportBeginPending) { logLine("[caw] Wait for the export to finish before locking the rack."); return false; }
    const std::vector<int> sources = freezeVsteSources(rackIdx);
    if (sources.empty()) { logLine("[caw] The rack has no renderable sources to capture."); return false; }
    // OFFLINE rack lock: possible when every source is a Csound / VSTi / sample / PD clip track that
    // the offline host can render (silent, full CPU speed) - or is ALREADY LOCKED (its rendered WAV
    // feeds the render through the offline sample streams, the host decodes it). SC sources keep
    // the real-time capture. A Csound source needs its language enabled; VSTi / sample sources
    // render offline even with the Csound engine unchecked. With the offline pass the render takes
    // seconds, not the wall-clock length of the range.
    bool offlineOk = !rack.vstPath.empty();
    for (int si : sources) {
        const Track& st = g_proj.tracks[(size_t)si];
        if (trackFrozen(st)) continue;   // a locked source: its frozen WAV feeds the offline render (engine-4 streams)
        if ((st.kind != 0 && st.kind != 3 && st.kind != 7 && st.kind != 6) ||
            (st.kind == 0 && !g_langCsound) ||
            (st.kind == 6 && [&] { if (!g_pdLibraryPath.empty()) return true; g_pdLibraryPath = plat::bundledLibpd(); return !g_pdLibraryPath.empty(); }() == false))
            { offlineOk = false; break; }
    }
    if (!offlineOk) {
    ensureSampleEngineForVstInputs();
    if (!g_vstEngineOn) { logLine("[caw] Locking the rack needs the VST engine: press Run VST first."); return false; }
    for (int si : sources) {
        const Track& st = g_proj.tracks[(size_t)si];
        if (trackFrozen(st)) {
            if (!g_langAudio) { logLine("[caw] Locking the rack needs Audio enabled: a rendered source feeds it through the sample engine."); return false; }
            continue;
        }
        if (st.kind == 0 && !engineIsUp()) { logLine("[caw] Locking the rack needs Csound (DSP) running (a source uses it)."); return false; }
        if (st.kind == 5 && !g_scReady) { logLine("[caw] Locking the rack needs SuperCollider running (a source uses it)."); return false; }
        if (st.kind == 6 && !g_pdEngineOn && !g_mixedPd) { logLine("[caw] Locking the rack needs Pure Data (DSP) running (a source uses it)."); return false; }
        if (st.kind == 7 && !g_langAudio) { logLine("[caw] Locking the rack needs Audio enabled (a live sample source uses it)."); return false; }
    }
    }
    double lo = 1e300, hi = 0.0;
    for (int si : sources) {
        const Track& st = g_proj.tracks[(size_t)si];
        if (trackFrozen(st)) {                                       // a locked source contributes its WAV range
            if (st.freezeEnd > st.freezeStart) { lo = std::min(lo, st.freezeStart); hi = std::max(hi, st.freezeEnd); }
            continue;
        }
        for (const Clip& c : st.clips) { lo = std::min(lo, c.start); hi = std::max(hi, c.start + c.dur); }
        for (const MidiClip& mc : st.midiClips) { lo = std::min(lo, mc.start); hi = std::max(hi, mc.start + mc.dur); }
    }
    if (hi <= lo) { logLine("[caw] The rack's sources have nothing to render (no clips)."); return false; }

    g_freeze.resetForNewPass();
    g_freeze.exportPass = g_exportBeginPending;   // an export-queue part: no lock at the end
    g_freeze.phase = 1;
    g_freeze.vsteBounce = true;
    g_freeze.trackIdx = rackIdx;
    g_freeze.trackId = rack.id;
    g_freeze.trackName = rack.name;
    g_freeze.t0 = std::max(0.0, lo);
    g_freeze.t1 = hi + 2.0;                                       // tail: plugin releases and delays ring out
    if (!g_freeze.exportPass) {
        // An LKD replaces the live rack: the tail must survive like the export's bounce does.
        g_freeze.t1 = hi + std::max(2.0, g_proj.cs.tail + 2.0);
    }
    if (g_freeze.exportPass) {   // the export range (Start/End) bounds the bounce exactly like the mix
        g_freeze.t0 = std::max(g_freeze.t0, g_exportOff.t0);
        // Ring to the end of the mix (see freezeTrackBegin): tails survive like the real-time capture.
        g_freeze.t1 = std::max(g_freeze.t1, g_exportOff.t1 + 2.0);
        if (g_freeze.t1 <= g_freeze.t0) {
            logLine("[caw] Export: the rack \"" + rack.name + "\" lies outside the export range - skipped.");
            return false;
        }
    }
    g_freeze.rate = std::max(8000, g_proj.cs.sr);
    freezeWavPaths(rack);
    std::string err;
    if (!g_freeze.wav.open(g_freeze.tmpPath, g_freeze.rate, 2, 2, err)) {
        g_freeze.error = err;
        g_freeze.phase = 0;
        logLine("[caw] Rack lock failed: " + err);
        return false;
    }

    // Save every mixer state the pass touches; the rack's input switches stay exactly as they are
    // (the sources must keep feeding the plugins during the capture).
    for (const Track& tr : g_proj.tracks) g_freeze.savedMuteSolo.push_back({tr.muted, tr.solo});
    for (const Track& tr : g_proj.tracks) if (tr.kind == 4) {
        std::array<bool, 8> on{};
        for (int s = 0; s < 8; ++s) on[(size_t)s] = tr.vstInputs[s].on;
        g_freeze.savedVstInputs.push_back(on);
    }
    g_freeze.savedLoopEnabled = g_proj.loopEnabled;
    g_freeze.savedLoopStart = g_proj.loopStart;
    g_freeze.savedLoopEnd = g_proj.loopEnd;
    g_freeze.savedMasterChain = g_masterChainEnabled.load(std::memory_order_relaxed);
    g_freeze.savedVolume = rack.volume;

    if (timelinePlaying()) toggleTimelinePlay();                  // the capture always plays from t0
    for (Track& tr : g_proj.tracks) { tr.muted = true; tr.solo = false; }
    for (int si : sources) {                                      // the sources stay live: the rack hears them
        g_proj.tracks[(size_t)si].muted = false;
        g_proj.tracks[(size_t)si].solo = false;
    }
    rack.muted = false;
    rack.solo = false;                                            // no solo: the replace-mode routing must see its sources audible
    rack.volume = 1.f;                                            // the rack fader stays live on the frozen WAV
    g_proj.loopEnabled = false;
    g_masterChainEnabled.store(false, std::memory_order_relaxed);
    pushVstActiveLive();                                          // the rack stays active: its sources are audible again

    // OFFLINE rack lock (PURE C++): the rack renders in CAW's own VST host (rack_offline.h) - the
    // same pointer-free path the live 4th engine uses. NO Csound in the bounce path: the offline
    // Csound host is what faulted inside csound7_64.dll during the bounce, and that crash
    // bypassed every handler (no report file was ever written). Requirements: the rack has a
    // plugin, and every source is LOCKED (its rendered WAV feeds through the streaming reader)
    // or is an unfrozen VSTi (the rack renders it natively, engine 3). Anything else falls back
    // to the real-time capture below.
    bool cppOk = !rack.vstPath.empty();
    bool pdLibAvailable = [&] { if (!g_pdLibraryPath.empty()) return true; g_pdLibraryPath = plat::bundledLibpd(); return !g_pdLibraryPath.empty(); }();
    for (int si : sources) {
        const Track& st = g_proj.tracks[(size_t)si];
        if (trackFrozen(st) || (st.kind == 3 && !st.vstPath.empty())) continue;   // locked: WAV stream; VSTi: rendered natively (engine 3)
        if (st.kind == 6 && pdLibAvailable) continue;                             // PD: rendered by the C++ PD host mixer (engine 1) - no freeze, no Csound
        cppOk = false;
        break;
    }
    if (offlineOk) {
        if (!cppOk) {
            logLine("[caw] Offline rack lock needs every source locked (LKD) first - an unlocked source's sound can only be rendered live. Falling back to the real-time capture.");
        } else {
        // PURE C++ OFFLINE RACK BOUNCE: the rack renders in CAW's own VST host (rack_offline.h),
        // the same pointer-free path the live 4th engine uses - no Csound anywhere in the render
        // path. Locked sources feed their rendered WAVs through the streaming reader; unfrozen
        // VSTi sources render natively (engine 3); the rack's and the VSTi sources' MIDI clips are
        // scheduled on the block clock. Falls back to the real-time capture on any problem.
        std::string error;
        std::map<int, cawcpp::RackCppRender::SourceStream> streams;
        for (int si : sources) {
            const Track& st = g_proj.tracks[(size_t)si];
            if (trackFrozen(st) && !st.freezePath.empty())
                streams[st.id] = { st.freezePath, st.freezeStart, std::max(0.f, std::min(kMaxVolumeGain, st.volume)) };   // the fader rides on top; the volume lane is baked into the WAV
        }
        // Render ONLY the rack and its sources: the job set stays small and honest.
        std::set<int> keepIds;   // freezeVsteSources returns INDICES - resolve them into track IDs first
        for (int si : sources)
            if (si >= 0 && si < (int)g_proj.tracks.size()) keepIds.insert(g_proj.tracks[(size_t)si].id);
        keepIds.insert(rack.id);
        Project renderProj = g_proj;
        {
            std::vector<Track> keep;
            for (const Track& tr : renderProj.tracks)
                if (keepIds.count(tr.id)) keep.push_back(tr);
            renderProj.tracks = std::move(keep);
        }
        vstui::captureAllParameters(g_proj);   // the render uses exactly what the plugin GUIs show
        std::vector<VstJob> jobs = collectVstJobs(renderProj, {});
        captureLiveVstInstanceState(jobs);   // the offline rack starts where the LIVE sound is
        for (VstJob& j : jobs)
            if (j.id == rack.id) j.outMode = 0;   // FX only for the render: the master carries the rack's wet output alone
        // Belt and braces: the render knows its own wiring exactly. Re-resolve the rack's ENABLED
        // input slots here (a locked source streams its WAV through engine 4; an unfrozen VSTi
        // source renders in this rack, engine 3; unfrozen PD renders in the C++ mixer, engine 1),
        // so the plugin hears the sources even if the shared route resolution came up empty.
        for (VstJob& j : jobs) {
            if (j.id != rack.id || !j.routedIn) continue;
            int cursor = 0;
            for (int s = 0; s < 8; ++s) {
                auto& in = j.inputs[s];
                if (!in.on || in.mode == 2 || in.name.empty()) continue;
                const Track* src = nullptr;
                for (const Track& o : renderProj.tracks) {
                    const bool match = in.mode == 0 ? o.name == in.name
                        : std::any_of(o.clips.begin(), o.clips.end(),
                            [&](const Clip& c) { return c.name == in.name; });
                    if (match) { src = &o; break; }
                }
                if (!src) { logLine("[caw] Offline render: the rack's input in" + std::to_string(s + 1) + " \"" + in.name + "\" matches no source track in the render."); continue; }
                in.lineStart = cursor;
                in.lineCount = 2;
                in.srcId = src->id;
                in.engine = trackFrozen(*src) ? 4 : (src->kind == 3 ? 3 : (src->kind == 6 ? 1 : (src->kind == 5 ? 2 : 0)));
                if (in.engine == 0) in.csChans.clear();   // a locked Csound source streams through engine 4 (cppOk guarantees locked)
                cursor += 2;
            }
        }
        if (jobs.empty()) error = "no renderable plugin set.";
        // MIDI: the rack's own clips and the unfrozen VSTi sources' clips, on the block clock.
        std::vector<cawcpp::RackCppRender::Note> notes;
        auto collectNotes = [&](const Track& t) {
            for (const MidiClip& mc : t.midiClips) {
                if (mc.muted) continue;
                for (const MidiNote& n : mc.notes) {
                    if (n.start >= mc.dur) continue;
                    cawcpp::RackCppRender::Note nn;
                    nn.start = mc.start + n.start;
                    nn.dur = std::max(0.005, std::min(mc.start + mc.dur, nn.start + n.dur) - nn.start);
                    nn.pitch = std::min(127, std::max(0, n.pitch));
                    nn.vel = std::min(127, std::max(1, n.velocity));
                    nn.trackId = t.id;
                    notes.push_back(nn);
                }
            }
        };
        collectNotes(rack);
        for (int si : sources) {
            const Track& st = g_proj.tracks[(size_t)si];
            if (!trackFrozen(st)) collectNotes(st);
        }
        g_freeze.csdPath.clear();
        g_freeze.offlineWavPath = g_freeze.tmpPath + ".full.wav";
        if (error.empty()) {
            if (error.empty()) {
            // Say exactly what the rack will hear, so a silent render is diagnosable from the console.
            for (const VstJob& j : jobs) if (j.id == rack.id) {
                std::string sum;
                for (int s = 0; s < 8; ++s) {
                    const auto& src = j.inputs[s];
                    if (!src.on || src.lineCount <= 0) continue;
                    sum += " in" + std::to_string(s + 1) + " \"" + src.name + "\" engine=" + std::to_string(src.engine) + " (" + std::to_string(src.lineCount) + " lines),";
                }
                logLine("[caw] Offline render: the rack's inputs:" + (sum.empty() ? " (none enabled)" : sum));
            }
        }
        // Unfrozen PD sources: the C++ PD host mixer renders their clips (engine 1 via pdMix).
            std::vector<pd_timeline::Event> pdPlan;
            std::string pdLib;
            bool pdNeeded = false;
            for (int si : sources)
                if (!trackFrozen(g_proj.tracks[(size_t)si]) && g_proj.tracks[(size_t)si].kind == 6) pdNeeded = true;
            if (pdNeeded) {
                int bad = 0; std::string pdErr;
                if (!pd_timeline::makePlan(renderProj, g_projectPath, pdPlan, pdErr, false, &bad, true))
                    error = "PD clip problem: " + pdErr;
                else
                    pdLib = pdLibAvailable ? g_pdLibraryPath : std::string();
            }
            if (error.empty()) {
                if (pdNeeded)
                    g_cppRender.startPd(std::move(jobs), std::move(streams), std::move(notes), g_freeze.rate,
                                        g_freeze.offlineWavPath, g_freeze.t1 + std::max(0.0, g_proj.cs.tail) + 1.0,
                                        std::move(pdLib), std::move(pdPlan), buildVstScripts(renderProj));
                else
                    g_cppRender.start(std::move(jobs), std::move(streams), std::move(notes), g_freeze.rate,
                                      g_freeze.offlineWavPath, g_freeze.t1 + std::max(0.0, g_proj.cs.tail) + 1.0,
                                      buildVstScripts(renderProj));
            }
        }
        if (error.empty()) {
            g_freeze.offline = true;
            g_freeze.cppOffline = true;
            g_freezeSilent.store(false, std::memory_order_release);   // nothing plays during the offline pass: no silence to keep
            g_freeze.phase = 3;
            g_freeze.startedAt = std::chrono::steady_clock::now();
            logLine("[caw] Locking the rack \"" + rack.name + "\": OFFLINE render of " +
                    std::to_string(g_freeze.t1 - g_freeze.t0) + " s in CAW's C++ VST host (no Csound in the render path) to " + g_freeze.finalPath);
            return true;
        }
        logLine("[caw] Offline rack lock not available (" + error + ") - falling back to the real-time capture.");
        g_freeze.offlineWavPath.clear();
        }
        if (!g_vstEngineOn) { logLine("[caw] Locking the rack needs the VST engine: press Run VST first."); return false; }
        for (int si : sources) {
            const Track& st = g_proj.tracks[(size_t)si];
            if (st.kind == 5 && !g_scReady) { logLine("[caw] Locking the rack needs SuperCollider running (a source uses it)."); return false; }
            if (st.kind == 6 && !g_pdEngineOn && !g_mixedPd) { logLine("[caw] Locking the rack needs Pure Data (DSP) running (a source uses it)."); return false; }
            if (st.kind == 7 && !g_langAudio) { logLine("[caw] Locking the rack needs Audio enabled (a live sample source uses it)."); return false; }
        }
    }

    g_freezeSilent.store(true, std::memory_order_release);   // the capture keeps working; the speakers stay silent
    freezeStartWriter();
    seekTimeline(g_freeze.t0);
    toggleTimelinePlay();
    logLine("[caw] Locking the rack \"" + rack.name + "\": rendering " + std::to_string(sources.size()) +
            " source(s) through its plugins, " + std::to_string(g_freeze.t1 - g_freeze.t0) + " s to " + g_freeze.finalPath);
    return true;
}
