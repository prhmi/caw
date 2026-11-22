#include "app_shared.h"

// src/engine_dispatch.cpp (phase 2: extracted from app.cpp; the shared state is app_state.h, the
//  internal declarations are app_internal.h - all included through app_shared.h.)

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
    audio.sampleRate = g_proj.cs.sr; audio.channels = 2;
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
    const int serverOutputBuses = physicalChannels + 2 * ScEngine::maxTrackSlots;
    std::ostringstream out;
    out << "(\n"
        << "o = ServerOptions.new; o.maxLogins = 32; o.numOutputBusChannels = " << serverOutputBuses
        << "; o.numInputBusChannels = 0; o.numAudioBusChannels = " << std::max(1024, serverOutputBuses + 256) << ";\n"
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
        << "OSCdef(\\cawStart, { |msg|\n"
        << "    var id = msg[1].asInteger, path = msg[2].asString, trackSlot = msg[7].asInteger, clipId = msg[8].asInteger;\n"
        << "    var delay = msg[3].asFloat.max(0), duration = msg[4].asFloat.max(0.001);\n"
        << "    var p4 = msg[5].asFloat, p5 = msg[6].asFloat, outBus = 0, generation;\n"
        << "    generation = ~cawgeneration;\n"
        << "    if ((trackSlot >= 0) and: { trackSlot < " << ScEngine::maxTrackSlots << " }) {\n"
        << "        outBus = ~cawtrackbuses[trackSlot] ?? { var index = " << physicalChannels << " + (trackSlot * 2); ~cawtrackbuses[trackSlot] = index; index; };\n"
        << "    };\n"
        << "    ~cawcancelled[id] = false;\n"
        << "    SystemClock.sched(delay, {\n"
        << "        var group;\n"
        << "        if ((generation == ~cawgeneration) and: { ~cawcancelled[id] != true }) {\n"
        << "            group = Group.tail(s); ~cawgroups[id] = group; ~cawgroup = group; ~cawoutbus = outBus;\n"
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
        << "~cawbuses = IdentityDictionary.new;   // one control bus per automation channel, CAW keeps it current like the lane dots\n"
        << "~cawbus = { |name| var key = name.asSymbol, bus = ~cawbuses[key]; if (bus.isNil) { bus = Bus.control(s, 1); bus.value = ~cawr[key] ? 0; ~cawbuses[key] = bus; }; bus; };   // cawbus: a live control bus for a lane, read it with In.kr(bus.index, 1)\n"
        << "OSCdef(\\cawHub, { |msg| var key = msg[1].asSymbol, value = msg[2], bus = ~cawbuses[key]; ~cawr[key] = value; if (bus.notNil) { bus.value = value; }; }, '/caw/hub');\n"
        << "~cawget = { |name| if (UGen.buildSynthDef.notNil) { In.kr(~cawbus.(name).index, 1) } { ~cawr[name.asSymbol] ? 0 } };   // cawget: inside a synth it is a live k-rate control signal on the lane bus (like Csound cawget); outside it reads the lane value once\n"
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
    static const std::regex directOut(R"re(\bOut\.ar\(\s*0\s*,)re");
    routedCode = std::regex_replace(routedCode, playTarget,
                                    ".play(outbus: ~cawoutbus, target: ~cawgroup");
    routedCode = std::regex_replace(routedCode, directOut, "Out.ar(~cawoutbus,");
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
        const bool audible = !t.muted && !(anySolo && !t.solo);
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
void pollCawHub() {
    static std::uint64_t seen = 0;
    std::map<std::string,double> values;
    if (!g_cawHub.take(seen, values)) return;
    if (!g_scEngine.running()) return;
    for (const auto& entry : values)
        g_scEngine.sendHub(entry.first, static_cast<float>(entry.second));
}
#include "app.h"
void pollScAutomation() {
    if (!g_scReady || !g_scEngine.running() || !g_timelineRun) return;
    for (const Track& t : g_proj.tracks) {
        if (t.kind != 5) continue;
        for (const auto& lane : t.automation) {
            const std::string& chn = lane.first;
            const std::vector<AutoPoint>& points = lane.second;
            if (points.empty() || !autoLaneActive(t, chn)) continue;
            double lo = 0.0, hi = 1.0, initial = 0.5;
            const auto range = t.automationRanges.find(chn);
            if (range != t.automationRanges.end()) {
                lo = range->second.min;
                hi = range->second.max > lo ? range->second.max : lo + 1.0;
                initial = range->second.value;
            }
            const float value = static_cast<float>(autoValueAt(points, g_play, initial));
            const bool first = g_scLaneSent.find(chn) == g_scLaneSent.end();
            float& seen = g_scLaneSent[chn];
            if (seen == value) continue;
            seen = value;
            if (first) logLine("[caw] SC automation: lane \"" + chn + "\" streams into the language (~cawget is live)");
            g_scEngine.sendHub(chn, value);
        }
    }
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
            for (int channel = 0; channel < 2; ++channel) {
                const float peak = g_scEngine.takeTrackPeak(slot, channel);
                const float db = 20.f * std::log10(std::max(peak, 1e-5f));
                meter[static_cast<size_t>(channel)] = std::max(meter[static_cast<size_t>(channel)], db);
            }
        }
    }
    if (g_sampleEngineOn) {
        const auto peaks = cawaudio::engine().takeTrackPeaks();   // trackId -> output channel peaks (max since the last frame)
        for (const Track& track : g_proj.tracks) {
            if (track.kind != 7 || trackHiddenByFolder(track)) continue;
            // how many meters this track shows: the widest output use of its clips (the saved routing
            // and the code's out1..out8 custom routing both count)
            int channels = 0;
            for (const Clip& c : track.clips) {
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
    if (g_vstSoloOutput && g_vstSoloCapture) {   // Run VST alone: its own stream feeds the master meter and the scope
        MonitorCapture::Snapshot snapshot;
        if (g_vstSoloCapture->take(snapshot, g_vstSoloSnapshotSeq)) {
            g_vstSoloSnapshotSeq = snapshot.sequence;
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
    // A master-mode ("FX only") VSTe that consumes a sample track replaces the raw sample mix:
    // silence the direct sample output while that VSTe runs (the effect's output is heard instead).
    {
        bool smpReplaced = false;
        bool anySoloT = false;
        for (const Track& t : g_proj.tracks) if (t.solo && !t.muted) anySoloT = true;
        // An effect belongs to its source's signal path (see collectVstActiveTracks): it replaces the
        // sample while THAT source is audible - soloing the source keeps its insert working.
        const auto srcAudible = [&](const Track& o) { return !o.muted && (!anySoloT || o.solo); };
        for (const Track& vt : g_proj.tracks) {
            if (vt.kind != 4 || vt.vstOutMode != 0 || vt.muted) continue;
            for (const auto& in : vt.vstInputs) {
                if (!in.on || in.mode == 2) continue;
                for (const Track& o : g_proj.tracks) {
                    if (o.kind != 7) continue;
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
                        if (o.kind != 7) continue;
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
    if (g_vstEngineOn) {   // the 4th engine's rack - it always hosts the VST graph (SEPARATED ENGINES)
        std::vector<VstRack::SoloPeak> peaks;
        vstSolo().takePeaks(peaks);
        for (const auto& p : g_apiProc.takeVstPeaks()) peaks.push_back({p.trackId, p.l, p.r});
        for(const auto& track:peaks) {
            auto it=g_vstMeterDb.find(track.trackId);
            if(it==g_vstMeterDb.end())it=g_vstMeterDb.emplace(track.trackId,std::array<float,2>{-100.f,-100.f}).first;
            const float channels[2] = {track.l, track.r};
            for(int ch=0;ch<2;++ch) {
                const float db=20.f*std::log10(std::max(channels[ch],1e-5f));
                it->second[ch]=std::max(it->second[ch],db);
            }
        }
    }
    if(g_pdEngineOn)for(const auto& track:g_pdTimelineEngine.takePeaks()) {
        auto it=g_pdMeterDb.find(track.trackId);
        if(it==g_pdMeterDb.end())it=g_pdMeterDb.emplace(track.trackId,std::array<float,2>{-100.f,-100.f}).first;
        for(int ch=0;ch<2;++ch) {
            const float db=20.f*std::log10(std::max(track.value[ch],1e-5f));
            it->second[ch]=std::max(it->second[ch],db);
        }
    }
    if(g_mixedPd)for(const auto& track:g_apiProc.takePdPeaks()) {
        auto it=g_pdMeterDb.find(track.trackId);
        if(it==g_pdMeterDb.end())it=g_pdMeterDb.emplace(track.trackId,std::array<float,2>{-100.f,-100.f}).first;
        for(int ch=0;ch<2;++ch) {
            const float db=20.f*std::log10(std::max(track.value[ch],1e-5f));
            it->second[ch]=std::max(it->second[ch],db);
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
        VstJob j; j.id = t.id; j.name = vstBusName(t); j.path = t.vstPath; j.classId = t.vstClassId;
        j.parameterState = t.vstParameterState;
        j.stateBlob = vstui::editorStateBlob(t.id);   // SEPARATED ENGINES: the editor instance's loaded patch (Kontakt .nki) - empty until captured
        if (j.stateBlob.empty()) j.stateBlob = t.vstPatchState;   // a project-saved patch state (v58) starts the fresh instance with the same library
        j.instrument = t.kind == 3; j.pick = t.vstPick; j.init = t.vstInit;
        j.outMode = t.vstOutMode == 1 ? 1 : 0;
        bool anyIn = false;
        for (int s = 0; s < 8; ++s) {
            j.inputs[s].on = t.vstInputs[s].on; j.inputs[s].mode = t.vstInputs[s].mode; j.inputs[s].name = t.vstInputs[s].name;
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
            if (const Track* o = owner(5, 6)) { srcId = o->id; return o->kind == 6 ? 1 : 2; }   // Pure Data / SuperCollider mix
            if (const Track* o = owner(3, 4)) { srcId = o->id; return 3; }                      // VSTi / VSTe output, wired by the rack
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
        for (const LaneMeterInfo& lm : meters) if (lm.trackId == t.id) { j.meterFirst = lm.first; break; }
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
                if ((vt.kind != 3 && vt.kind != 4) || vt.muted || (anySolo && !vt.solo)) continue;   // not a VST track: the Csound path plays it
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
std::vector<VstScript> buildVstScripts() {
    std::vector<VstScript> out;
    bool anySolo = false;
    for (const Track& t : g_proj.tracks) if (t.solo && !t.muted) anySolo = true;
    for (const Track& t : g_proj.tracks) {
        if ((t.kind != 3 && t.kind != 4) || t.muted || (anySolo && !t.solo)) continue;   // VSTi (3) AND VSTe (4): the clip code runs in C++ here
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
        if ((t.kind == 3 || t.kind == 4) && !t.vstPath.empty() && !t.muted && (!anySolo || t.solo))
            ids.insert(t.id);
    // An EFFECT wired to an audible source keeps processing it even while ANOTHER track is soloed:
    // it is that source's insert, so it belongs to the source's signal path - exactly like the
    // master FX chain, which the track M / S never touches. Without this, soloing the source track
    // (the natural way to audition it) silently skipped every VSTe wired to it: the source played
    // dry and the plugin "did nothing". Repeated so an effect chain follows its sources.
    const auto srcAudible = [&](const Track& o) {
        if (o.muted) return false;
        if (o.kind == 3 || o.kind == 4) return ids.count(o.id) != 0;   // a VST source follows its own chain
        return !anySolo || o.solo;
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
void pushVstActiveLive() {
    if (g_liveCsound && g_useCsoundApi) g_apiProc.setVstActiveTracks(collectVstActiveTracks(g_proj));
    if (g_vstEngineOn) vstSolo().setActive(vstSoloActiveIds());
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
    static VstEngineAudio soloAudio;   // the SuperCollider mix is captured by CAW's callback (g_scRing)
    soloAudio.scRing = &g_scRing;
    soloAudio.pdMix = nullptr;         // the Pure Data mix has no app-side ring: a PD input needs Csound
    soloAudio.sampleRing = [](int trackId) -> CawScRing* { return sampleRingOf(trackId); };   // a sample track's stereo mix (engine 4)
    if (!vstSolo().start(jobs, audio.sampleRate, 2, 512, &soloAudio)) return false;
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
                else if (src.engine == 1) { sum += "\"" + src.name + "\" (PureData - needs Csound)"; csInput = true; }
                else if (src.engine == 2) sum += "\"" + src.name + "\" (SuperCollider mix)";
                else if (src.engine == 3) sum += "\"" + src.name + "\" (VST output)";
                else if (src.engine == 4) sum += "\"" + src.name + "\" (sample track mix)";
                else { sum += "\"" + src.name + "\" (Csound track - needs Csound)"; csInput = true; }
            }
            logLine("[caw] VSTe \"" + j.name + "\" runs in the VST engine; inputs: " +
                    (sum.empty() ? std::string("(none enabled - nothing to process)") : sum) + ".");
            if (csInput)
                logLine("[caw] VSTe \"" + j.name + "\": a Csound / PD track only sounds while Csound runs. "
                        "With the VST engine alone, wire the effect to a VSTi track, the master, or the SuperCollider mix.");
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
    if (vstSolo().sameJobs(jobs)) { g_vstFailedSig.clear(); return; }
    if (jobs.empty()) {
        g_vstFailedSig.clear();
        if (vstSolo().ready()) {   // the last VST track / plugin went away: unload, keep the button on and wait
            vstSolo().unload();
            vstSolo().setEngineOn(true);   // Run VST stays on: a plugin loaded later starts playing by itself
            g_vstMeterDb.clear();
            logLine("[caw] VST engine: no plugin now - waiting (Run VST stays on).");
        }
        return;
    }
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
    g_engineMeters = meterLayout(g_proj);
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
    g_tonePeaks.clear();
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
        if (g_proj.cs.inDev >= 0 || g_proj.cs.options.find("-iadc") != std::string::npos) {
            logLine("[caw] CAW-owned audio input is not available yet. Use run_osc_monitor.bat for audio input.");
            g_cawMonitorApi = false; g_useCsoundApi = false;
            return;
        }
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
void cleanupRenderTemps() {
    std::error_code ec;
    if (!g_renderTemp.empty()) std::filesystem::remove(g_renderTemp, ec);
    if (!g_renderEncodedTemp.empty()) std::filesystem::remove(g_renderEncodedTemp, ec);
    g_renderTemp.clear(); g_renderEncodedTemp.clear();
}
#include "app.h"
void startRenderNow() {
    if (!g_langCsound) { logLine("[caw] PD audio export is not available yet. Enable Csound to export Csound projects."); return; }
    if (g_pdEngineOn || g_pdPreview.active()) { logLine("[caw] Stop Pure Data playback before exporting."); return; }
    if (g_renderActive) return;
    vstui::captureParameters(g_proj);
    g_exportError.clear();
    const int format = std::min(4, std::max(0, g_proj.cs.renderFormat));
    const int channels = g_proj.cs.exportChannels;
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
    for (const Track& track : g_proj.tracks)
        if (!track.muted && (track.kind == 5 || track.kind == 6) &&
            std::any_of(track.clips.begin(), track.clips.end(), [](const Clip& c) { return !c.muted; })) {
            g_exportError = "Audio export cannot include PD or SuperCollider clips yet.";
            logLine("[caw] " + g_exportError);
            return;
        }
    for (const Track& track : g_proj.tracks)
        if (!track.muted && (track.kind == 3 || track.kind == 4) && !track.clips.empty() && track.vstPath.empty()) {
            g_exportError = "VST track " + track.name + " has clips but no plugin selected.";
            logLine("[caw] " + g_exportError);
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
    const int wavFormat = format >= 3 ? 2 : format;
    if (!g_offlineProc.startOffline(csdPath, g_proj.cs.sr, masterChannels(g_proj.cs),
                                    (float)g_proj.cs.zdbfs, g_proj.cs.masterGain,
                                    wavFormat, g_renderTemp, error, channels)) {
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
    g_renderWarnVst = false;
    for (const Track& track : g_proj.tracks)
        if ((track.kind == 3 || track.kind == 4) && !track.muted && !track.vstPath.empty())
            g_renderWarnVst = true;
    if (g_renderWillOverwrite || g_renderWarnVst) g_confirmRenderOverwrite = true;
    else startRenderNow();
}
#include "app.h"
void cancelRender() {
    g_offlineProc.stop();
    g_codecProc.stop();
    g_renderProc.stop();
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
    spec.gain0 = c.audioGain * (double)std::max(0.f, std::min(2.f, trackVolume));   // the track volume slider
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
    } else if (!g_timelineRun) {
        if (!g_sampleEngineOn) { playNotice("Press DSP first, then Run clip to audition.", "Sample engine is off"); return; }
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
void scheduleAudioTimeline(double from, bool restart) {
    cawaudio::Engine& eng = cawaudio::engine();
    eng.setSr(g_proj.cs.sr);
    if (!g_timelineRun || !g_sampleEngineOn) { eng.clearAll(); g_audioVoiceSig.clear(); g_audioLastFrom = from; return; }
    if (restart) { eng.clearAll(); g_audioVoiceSig.clear(); g_audioLastFrom = from; }
    bool anySolo = false;
    for (const Track& t : g_proj.tracks) if (t.solo && !t.muted) anySolo = true;
    std::set<int> want;
    for (size_t ti = 0; ti < g_proj.tracks.size(); ++ti) {
        const Track& t = g_proj.tracks[ti];
        if (t.kind != 7 || trackHiddenByFolder(t)) continue;
        const bool audible = !t.muted && !(anySolo && !t.solo);
        eng.setTrackGain(t.id, trackVolumeAutomation(t, from));   // the volume lane is live (0 = silent)
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
    for (auto it = g_audioVoiceSig.begin(); it != g_audioVoiceSig.end();)
        if (want.count(it->first)) ++it; else { eng.killManaged(it->first); it = g_audioVoiceSig.erase(it); }

    // sampler notes: the notes of the MIDI lane trigger the clip like a sample
    const double lookahead = engineOutputLatency() + 0.06;
    for (size_t ti = 0; ti < g_proj.tracks.size(); ++ti) {
        const Track& t = g_proj.tracks[ti];
        if (t.kind != 7 || trackHiddenByFolder(t) || t.midiClips.empty()) continue;
        const bool audible = !t.muted && !(anySolo && !t.solo);
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
    ensureSampleOutput();   // the sample engine keeps an output stream open whenever nothing else owns the card
    pollScAutomation();   // SuperCollider lanes follow the playhead (~cawget in the language)
    if (timeline_loop::valid(g_proj) && g_play >= g_proj.loopEnd) {
        g_play = timeline_loop::wrap(g_proj, g_play);
        // This host reissues transport commands at the boundary. It is not a
        // sample-accurate or gapless loop, but nothing beyond the end is queued.
        if (engineIsUp()) commandPlayFrom(g_play, true);
        if (scUp) scheduleScTimeline(g_play, true);
        scheduleAudioTimeline(g_play, true);
        if (audioOnly) audioClockSync(g_play);
        mixedPdTransport(true);
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
    diagClearClip(clip.name);
    g_testProc.stop();
    g_test = TestRun();
    g_test.name = clip.name;
    const std::string csdPath = (workDir() / "caw_test.csd").string();
    OrcSpan span;
    const std::string csd = buildClipTestCsd(g_proj, clip, span);
    g_orcSpans.assign(1, span);
    if (!writeTextFile(csdPath, csd)) {
        logLine("[caw] ERROR: could not write " + csdPath);
        return;
    }
    logLine("[caw] Run clip " + clip.name + " (this clip alone, no audio)...");
    if (!g_testProc.start("\"" + csoundExePath() + "\" \"" + csdPath + "\"", false))
        logLine("[caw] ERROR: " + g_testProc.lastError());
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
    const bool running = scClipRunning(clip.id);
    if (inWindow) {
        if (running) stopScClip(clip.id);
        launchScClip(clip, clip.start + clip.dur - g_play);   // re-evaluate for the remaining window
    } else if (!g_timelineRun) {
        if (running) stopScClip(clip.id);   // a fresh audition with the new code
        launchScClip(clip);
        g_scManualRuns[clip.id] = std::chrono::steady_clock::now() +
            std::chrono::milliseconds((long long)(clip.dur * 1000.0 + 500.0));   // the button shows it (manual audition)
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
        logLine("[caw] Enable the VST engine to apply this clip's code.");
        return false;
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
    if (g_offlineProc.active()) {
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
                if (!g_codecProc.start(g_renderTemp, g_renderEncodedTemp, csoundExePath(),
                                       g_activeRenderFormat, g_activeFlacBits, g_activeMp3Kbps) &&
                    !startFallbackEncoder()) {
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
