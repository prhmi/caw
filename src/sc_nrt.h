#pragma once

// CAW's offline SuperCollider freeze, without Csound anywhere in the render path.
//
// Two processes do the work:
//   1. sclang (the CAW private class library + the CAWNrt class below) runs every clip's code
//      in VIRTUAL time: the patched SystemClock / TempoClock queue the clip's scheduling into
//      the CAWNrt queue and every OSC send the clip makes is recorded into a Score at the
//      virtual time it would have played. The mock server's address is a CawScoreAddr, so no
//      OSC leaves the capture process and no live server is needed.
//   2. scsynth -N renders the score file faster than real time into the intermediate WAV
//      (project time 0 to the render end), exactly like the other offline renderers; the
//      freeze finisher slices [t0, t1] out of it.
//
// The class library (CAWNrt.sc) is embedded here and written next to sclang.exe, because class
// extensions cannot be loaded at runtime - the files must exist when sclang compiles its library.

#include "process_quote.h"
#include "process_runner.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace scnrtext {

inline const char* cawNrtClass = R"SCTEXT(// CAW NRT capture: virtual-clock recording of SuperCollider clip code into an OSC score.
// The overrides are gated on CAWNrt.active - the live engine runs the original implementations.
// The mock server uses a CawScoreAddr, so NO OSC ever leaves sclang during the capture.

CawScoreAddr {
	// The mock server's address: every OSC send is recorded into the score at the current
	// virtual time. A real NetAddr's methods, but recording instead of sending.
	sendMsg { |...args| CAWNrt.addMsg(0.0, args.asArray) }
	sendBundle { |time ...args| CAWNrt.addBundle(time ? 0.0, args) }
	sendClumpedBundles { |time ...args| CAWNrt.addBundle(time ? 0.0, args) }
	sendRaw { |raw| CAWNrt.addMsg(0.0, raw.asArray) }
	sendStatusMsg { }
	hasBundle { ^false }
	sync { |condition, bundles, latency| }
	isLocal { ^true }
	addr { ^nil }   // Server:addr_ checks addr.addr for the in-process flag: nil keeps it false
	matches { |that| ^false }
	ip { ^"127.0.0.1" }
	port { ^1 }
	printOn { |stream| stream << "CawScoreAddr" }
	storeOn { |stream| stream << "CawScoreAddr()" }
}

CAWNrt {
	classvar <numCmds, <active = false, <score, <time, <queue, seq = 0, <doneTime;


	*reset {
		score = Score.new; time = 0.0; queue = PriorityQueue.new; seq = 0; doneTime = nil; active = true;
	}

	// the Score element shape is [time, cmdList]: makeSynthBundle treats every
	// remaining slot of the element as ONE message, so cmdList IS the message.
	*addMsg { |bundleTime, msgArray|
		score = score.add([time + bundleTime, this.normMsg(msgArray)])
	}
	// sendBundle's rest args are the messages one per arg: the Score element spreads them
	// ([time, msg1, msg2, ...]) - wrapping them again would nest one level too deep and the
	// NRT command reader dies on the mangled element.
	*addBundle { |bundleTime, msgs|
		score = score.add([time + bundleTime] ++ msgs.asArray.collect { |m| this.normMsg(m) })
	}
	// sclang sends OSC commands as NUMERIC OPCODES (Server.sc: sendMsg(9, ...)).
	// The NRT command reader mis-dispatches them, so capture with slash addresses.
	*normMsg { |msgArray|
		var n = msgArray[0];
		^if (n.isInteger and: { n >= 0 and: { n < this.numCmds.size } }) {
			this.numCmds[n] !? { (("/" ++ this.numCmds[n]).asSymbol.asArray ++ msgArray[1..]).asArray } ?? msgArray
		} { msgArray }
	}
	*initClass {
		numCmds = [\none, \notify, \status, \quit, \cmd, \d_recv, \d_load, \d_loadDir, \d_freeAll,
			\s_new, \n_trace, \n_free, \n_run, \n_cmd, \n_map, \n_set, \n_setn, \n_fill,
			\n_before, \n_after, \u_cmd, \g_new, \g_head, \g_tail, \g_freeAll,
			\c_set, \c_setn, \c_fill, \b_alloc, \b_allocRead, \b_read, \b_write,
			\b_free, \b_close, \b_zero, \b_set, \b_setn, \b_fill, \b_gen, \dumpOSC,
			\c_get, \c_getn, \b_get, \b_getn, \s_get, \s_getn, \n_query, \b_query,
			\n_mapn, \s_noid, \g_deepFree, \clearSched, \sync, \d_free, \b_allocReadChannel,
			\b_readChannel, \g_dumpTree, \g_queryTree, \error, \s_newargs, \n_mapa, \n_mapan,
			\n_order, \p_new, \version, \rtMemoryStatus, \b_setSampleRate];
		score = Score.new; time = 0.0; queue = PriorityQueue.new;
	}

	*sched { |delta, item|
		var d = delta;
		if (d.notNil and: { d.isNumber.not }) { d = 0 };   // a Quant object: virtual time has no grid, start now
		if (d.notNil) { queue.put(time + d, item) };
	}
	*schedAbs { |atTime, item| queue.put(atTime, item) }
	*play { |item, quant| this.sched(0.0, item) }   // quant is a grid alignment: start now in virtual time

	*isEmpty { ^queue.isEmpty }

	*runOne { |item|
		var delta = nil;
		if (item.isKindOf(Routine)) {
			delta = item.next(SystemClock);
		} {
			if (item.respondsTo(\awake)) {
				delta = item.awake(time, time, SystemClock);
			} {
				delta = item.value;
			};
		};
		if (delta.isNumber and: { delta >= 0 }) {
			queue.put(time + delta, item);
		};
	}

	*run { |stopTime|
		while {
			queue.notEmpty and: { stopTime.isNil or: { queue.topPriority <= stopTime } }
		} {
			time = queue.topPriority;
			this.runOne(queue.pop);
		};
		doneTime = time;
	}

	*write { |path, endTime|
		var end = doneTime ? time;
		if (endTime.notNil and: { endTime.isNumber and: { endTime > end } }) { end = endTime };
		score.add([end, [0]]); score.sort; score.write(path);
	}

	*finish { active = false; }
}

// SynthDef.add() without arguments sends to all BOOTED servers - there are none in the
// capture, so the def would never be recorded. Divert to the mock server while CAWNrt runs;
// otherwise run the original body verbatim.
+ SynthDef {
	add { |libname, completionMsg, keepDef = true|
		if (CAWNrt.active and: { libname.isNil or: { libname.isKindOf(Server) } }) {
			^this.send(libname ?? Server.default, completionMsg, keepDef)
		};
		^this.prRealtimeAdd(libname, completionMsg, keepDef)
	}
	prRealtimeAdd { |libname, completionMsg, keepDef = true|
		var servers, desc = this.asSynthDesc(libname ? \global, keepDef);
		if (libname.isNil) {
			servers = Server.allBootedServers
		} {
			servers = SynthDescLib.getLib(libname).servers
		};
		servers.do { |each|
			this.doSend(each.value, completionMsg.value(each))
		}
	}
}

+ Server {
	serverRunning { ^if (CAWNrt.active) { true } { statusWatcher.serverRunning } }
	hasBooted { ^if (CAWNrt.active) { true } { statusWatcher.hasBooted } }
}

// SystemClock.sched: the capture diverts BEFORE the real-time scheduler primitive.
// SystemClock is a class with no instances: the patched methods are CLASS methods.
+ SystemClock {
	*sched { |delta, item|
		if (CAWNrt.active) { CAWNrt.sched(delta, item); ^nil };
		^this.prRealtimeSched(delta, item)
	}
	*schedAbs { |atTime, item|
		if (CAWNrt.active) { CAWNrt.schedAbs(atTime, item); ^nil };
		^this.prRealtimeSchedAbs(atTime, item)
	}
	*prRealtimeSched { |delta, item|
		_SystemClock_Sched
		^this.primitiveFailed
	}
	*prRealtimeSchedAbs { |atTime, item|
		_SystemClock_SchedAbs
		^this.primitiveFailed
	}
}

// TempoClock: instance scheduling is also a primitive, so the same delegate pattern applies.
+ TempoClock {
	sched { |delta, item|
		if (CAWNrt.active) { CAWNrt.sched(delta, item); ^nil };
		^this.prRealtimeSched(delta, item)
	}
	schedAbs { |beat, item|
		if (CAWNrt.active) { CAWNrt.schedAbs(beat, item); ^nil };
		^this.prRealtimeSchedAbs(beat, item)
	}
	play { |task, quant = 1|
		if (CAWNrt.active) { CAWNrt.play(task, quant); ^nil };
		^this.prRealtimePlay(task, quant)
	}
	prRealtimeSched { |delta, item|
		_TempoClock_Sched
		^this.primitiveFailed
	}
	prRealtimeSchedAbs { |beat, item|
		_TempoClock_SchedAbs
		^this.primitiveFailed
	}
	prRealtimePlay { |task, quant = 1|
		this.prRealtimeSchedAbs(quant.nextTimeOnGrid(this), task);
	}
}
)SCTEXT";

}  // namespace scnrtext

namespace scnrt {

// One clip start in the capture: the clip's own code file plays at its absolute timeline
// position (delay from project time 0), for its clip duration.
struct ScStart {
    double delay = 0.0, dur = 0.05, p4 = 0.0, p5 = 0.0;
    int id = 0, clipId = 0, slot = -1;
    std::string path;
};

// One automation lane of the frozen track, sampled in C++ (autoValueAt): ~cawget reads it
// live through the hub in real time, so the capture schedules the samples into virtual time.
struct ScLane {
    std::string name;
    std::vector<std::pair<double, double>> steps;   // (absolute time, value), only the changes
};

inline std::string scEscape(std::string s) {
    std::string out;
    for (char ch : s) {
        if (ch == '"' || ch == 92) out += (char)92;
        out += ch;
    }
    return out;
}

inline std::string fnum(double v) {
    std::ostringstream o;
    o << v;
    return o.str();
}

// The capture script: the mock server, the same clip dispatch the live /caw/start OSCdef
// runs (identical body, outBus forced to 0 so the render's stereo buses ARE the track),
// one virtual-time start per clip, then write the score and exit.
inline std::string captureScript(const std::vector<ScStart>& starts,
                                 const std::vector<ScLane>& lanes,
                                 const std::string& scorePath, double renderEnd) {
    std::ostringstream out;
    out << "(\n"
        << "~cawsrv = Server(\\cawnrt, CawScoreAddr.new);\n"
        << "~cawsrv.options = ServerOptions.new; ~cawsrv.options.maxLogins = 32;\n"
        << "~cawsrv.clientID = 0;\n"
        << "~cawsrv.newNodeAllocators; ~cawsrv.newBusAllocators; ~cawsrv.newBufferAllocators; ~cawsrv.newScopeBufferAllocators;\n"
        << "Server.default = ~cawsrv;\n"
        << "CAWNrt.reset;\n"
        << "~cawserveraddr = CawScoreAddr.new;\n"
        << "~cawgroups = IdentityDictionary.new; ~cawcancelled = IdentityDictionary.new; ~cawgeneration = 0;\n"
        << "~cawtrackbuses = IdentityDictionary.new; ~cawr = IdentityDictionary.new; ~cawbuses = IdentityDictionary.new;\n"
        << "~cawbus = { |name| var key = name.asSymbol, bus = ~cawbuses[key]; if (bus.isNil) { bus = Bus.control(s, 1); bus.value = ~cawr[key] ? 0; ~cawbuses[key] = bus; }; bus; };\n"
        << "~cawget = { |name| if (UGen.buildSynthDef.notNil) { In.kr(~cawbus.(name).index, 1) } { ~cawr[name.asSymbol] ? 0 } };\n"
        << "~cawsetlane = { |name, value| ~cawr[name.asSymbol] = value; ~cawbus.(name).value = value; };   // the lane automation, recorded at its virtual time\n"
        << "~cawlanelook = IdentityDictionary.new;\n"
        << "~cawstart = { |msg|\n"
        << "    var id = msg[1].asInteger, path = msg[2].asString, clipId = msg[8].asInteger;\n"
        << "    var duration = msg[4].asFloat.max(0.001);\n"
        << "    var p4 = msg[5].asFloat, p5 = msg[6].asFloat, outBus = 0, generation;\n"
        << "    generation = ~cawgeneration;\n"
        << "    ~cawcancelled[id] = false;\n"
        << "    SystemClock.sched(0, {\n"
        << "        var group;\n"
        << "        if ((generation == ~cawgeneration) and: { ~cawcancelled[id] != true }) {\n"
        << "            group = Group.tail(s); ~cawgroups[id] = group; ~cawgroup = group; ~cawoutbus = outBus;\n"
        << "            ~cawp4 = p4; ~cawp5 = p5; ~cawnoteduration = duration;\n"
        << "            (\"CAW_SC_CLIP_BEGIN \" ++ id.asString ++ \" \" ++ clipId.asString).postln;\n"
        << "            try { thisProcess.interpreter.executeFile(path); } { |error| (\"CAW_SC_CLIP_ERROR \" ++ id.asString ++ \" \" ++ error.asString).postln; error.reportError; };\n"
        << "            (\"CAW_SC_CLIP_END \" ++ id.asString).postln;\n"
        << "            SystemClock.sched(duration, { group.free; ~cawgroups.removeAt(id); ~cawcancelled.removeAt(id); nil; });\n"
        << "        };\n"
        << "        nil;\n"
        << "    });\n"
        << "};\n"
        ;
    for (const ScLane& L : lanes) {
        if (L.steps.empty()) continue;
        // The lane is DATA (a [time, value] array) + one self-rescheduling applier: hundreds of
        // individual schedAbs statements in the script body overflow sclang's per-function
        // selector table ("Selector table too big") and the capture script would not compile.
        out << "~cawlanelook[\"" << scEscape(L.name) << "\"] = [";
        for (size_t i = 0; i < L.steps.size(); ++i) {
            if (i) out << ", ";
            out << "[" << fnum(L.steps[i].first) << ", " << fnum(L.steps[i].second) << "]";
        }
        out << "];\n";
    }
    if (!lanes.empty()) {
        out << "~cawlaneapply = { |name, steps, index|\n"
            << "    var step = steps[index];\n"
            << "    if (step.notNil) {\n"
            << "        CAWNrt.schedAbs(step[0], { ~cawsetlane.value(name, step[1]); ~cawlaneapply.value(name, steps, index + 1); nil; });\n"
            << "    };\n"
            << "};\n"
            << "~cawlanelook.keysValuesDo { |name, steps| if (steps.notEmpty) { ~cawlaneapply.value(name, steps, 0) } };\n";
    }
    for (const ScStart& s : starts) {
        out << "CAWNrt.sched(" << fnum(s.delay) << ", { ~cawstart.value(['/caw/start', "
            << s.id << ", \"" << scEscape(s.path) << "\", 0.0, " << fnum(s.dur) << ", "
            << fnum(s.p4) << ", " << fnum(s.p5) << ", " << s.slot << ", " << s.clipId << "]); });\n";
    }
    out << "CAWNrt.run(nil);\n"
        << "(\"CAW_SC_NRT_EVENTS \" ++ CAWNrt.score.score.size ++ \" virtual end \" ++ CAWNrt.time).postln;\n"
        << "CAWNrt.write(\"" << scEscape(scorePath) << "\", " << fnum(renderEnd) << ");\n"
        << "\"CAW_SC_NRT_SCORE\".postln;\n"
        << "0.exit;\n"
        << ")\n";
    return out.str();
}

// Writes CAWNrt.sc into <sclang dir>/SCClassLibrary/CAW/ when it is missing or stale.
// Returns false when the folder could not be reached (the caller falls back to real time).
inline bool installClassLibrary(const std::string& sclangExe, std::string& err) {
    try {
        const auto dir = std::filesystem::u8path(sclangExe).parent_path() / "SCClassLibrary" / "CAW";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec || !std::filesystem::is_directory(dir, ec)) {
            err = "CAW's SuperCollider class folder is not writable: " + dir.u8string();
            return false;
        }
        const auto path = dir / "CAWNrt.sc";
        bool stale = true;
        {
            std::ifstream in(path, std::ios::binary);
            if (in) {
                std::string existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                stale = existing != scnrtext::cawNrtClass;
            }
        }
        if (stale) {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file) {
                err = "could not write " + path.u8string();
                return false;
            }
            file << scnrtext::cawNrtClass;
        }
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

// The worker: sclang captures the score, scsynth -N renders it. Polls like the other
// offline renderers (done/code); freezeCancel sets stop, which kills the child process.
struct ScNrtRender {
    std::atomic<bool> done{false}, stop{false};
    std::atomic<int> code{-1};
    std::mutex logMutex;
    std::vector<std::string> log;
    std::string error;
    // config (set before start)
    std::string sclangCmd, scsynthCmd, scorePath, outWavPath, trackName;
    double sclangTimeout = 180.0, scsynthTimeout = 180.0;

    void say(const std::string& s) { std::lock_guard<std::mutex> l(logMutex); if (log.size() < 512) log.push_back(s); }
    std::vector<std::string> takeLog() { std::lock_guard<std::mutex> l(logMutex); std::vector<std::string> out; log.swap(out); return out; }
    void setError(const std::string& e) { std::lock_guard<std::mutex> l(logMutex); error = e; }
    std::string takeError() { std::lock_guard<std::mutex> l(logMutex); std::string e; error.swap(e); return e; }
    void finish(int c) { code.store(c, std::memory_order_release); done.store(true, std::memory_order_release); }

    void start(const std::string& trackName_, const std::string& sclangCmd_, const std::string& scsynthCmd_,
               const std::string& scorePath_, const std::string& outWavPath_, double sclangTimeout_,
               double scsynthTimeout_) {
        trackName = trackName_;
        sclangCmd = sclangCmd_; scsynthCmd = scsynthCmd_;
        scorePath = scorePath_; outWavPath = outWavPath_;
        sclangTimeout = sclangTimeout_; scsynthTimeout = scsynthTimeout_;
        done.store(false, std::memory_order_release); stop.store(false, std::memory_order_release); code.store(-1, std::memory_order_release);
        std::thread([this] { run(); }).detach();   // freezeTick polls done/code; freezeCancel sets stop
    }

    void pump(ProcessRunner& r, const char* stage) {
        for (const std::string& l : r.takeOutput()) {
            say(std::string(stage) + ": " + l);
            // A sclang compile error ("Error: parsing error", "Selector table too big", ...) leaves
            // the interpreter alive: the capture can never succeed, so fail the pass now instead of
            // running out the whole timeout. Clip errors are prefixed CAW_SC_CLIP_ERROR and continue.
            if (l.rfind("Error:", 0) == 0) {
                r.stop();
                setError(std::string(stage) + " failed to compile: " + l);
            }
        }
    }

    bool runTo(const std::string& cmd, double cap, const char* stage, ProcessRunner& r) {
        if (!r.start(cmd)) {
            setError(std::string(stage) + " could not start: " + r.lastError());
            say("[caw] Offline render: " + std::string(stage) + " could not start.");
            return false;
        }
        const auto begin = std::chrono::steady_clock::now();
        while (true) {
            pump(r, stage);
            {
                std::lock_guard<std::mutex> l(logMutex);
                if (!error.empty()) break;   // the pump failed the pass (compile error)
            }
            if (!r.running()) break;
            if (stop.load(std::memory_order_relaxed)) {
                r.stop();
                say("[caw] Offline render: cancelled while " + std::string(stage) + " ran.");
                return false;
            }
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
            if (elapsed > cap) {
                r.stop();
                setError(std::string(stage) + " did not finish within " + fnum(cap) + " s (timed out).");
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        pump(r, stage);
        {
            std::lock_guard<std::mutex> l(logMutex);
            if (!error.empty()) return false;   // the pump already set a specific error
        }
        if (r.exitCode() != 0) {
            setError(std::string(stage) + " failed (exit code " + std::to_string(r.exitCode()) + ").");
            return false;
        }
        return true;
    }

    void run() {
        ProcessRunner lang;
        if (!runTo(sclangCmd, sclangTimeout, "sclang capture", lang)) { finish(-1); return; }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(std::filesystem::u8path(scorePath), ec) || ec) {
            setError("the capture produced no score file: " + scorePath);
            finish(-1);
            return;
        }
        ProcessRunner synth;
        if (!runTo(scsynthCmd, scsynthTimeout, "scsynth NRT", synth)) { finish(-1); return; }
        if (!std::filesystem::is_regular_file(std::filesystem::u8path(outWavPath), ec) || ec ||
            std::filesystem::file_size(std::filesystem::u8path(outWavPath), ec) < 44) {
            setError("the NRT render produced no audio (the output WAV is missing or empty).");
            finish(-1);
            return;
        }
        say("[caw] Offline render: SuperCollider NRT render finished normally.");
        finish(0);
    }
};

}  // namespace scnrt
