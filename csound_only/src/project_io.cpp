#include "project_io.h"
#include "csd_builder.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>

namespace {
void writeString(std::ostream& out, const std::string& value) {
    out << value.size() << '\n';
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
    out << '\n';
}

bool readString(std::istream& in, std::string& value) {
    size_t size = 0;
    if (!(in >> size) || size > 64 * 1024 * 1024) return false;
    in.get();
    value.assign(size, '\0');
    if (size && !in.read(&value[0], static_cast<std::streamsize>(size))) return false;
    if (in.get() != '\n') return false;
    return true;
}
}

namespace {
void writeProject(std::ostream& out, const Project& p) {
    const CsoundSettings& s = p.cs;
    out << "CAW_PROJECT 38\n"
        << s.sr << ' ' << s.ksmps << ' ' << s.nchnls << ' ' << s.nchnlsIn << ' '
        << std::setprecision(17) << s.zdbfs << ' ' << s.a4 << ' ' << s.bufSW << ' ' << s.bufHW << ' '
        << s.outDev << ' ' << s.inDev << ' ' << s.renderFormat << ' ' << s.tail << ' '
        << s.editorFontSize << ' ' << s.editorFontStyle << '\n';
    writeString(out, s.rtaudio);
    writeString(out, s.csoundExe);
    writeString(out, s.renderFile);
    writeString(out, s.options);
    writeString(out, std::string());   // was the Pure Data program (field kept so the file format stays the same)
    writeString(out, std::string());   // was the SuperCollider program
    out << s.syncMs << '\n';
    out << p.tracks.size() << '\n';
    for (const Track& t : p.tracks) {
        out << "TRACK " << t.id << ' ' << (t.muted ? 1 : 0) << ' ' << (t.solo ? 1 : 0) << ' '
            << std::setprecision(9) << t.height << ' ' << t.color << '\n';
        writeString(out, t.name);
        writeString(out, t.code);
        out << t.clips.size() << '\n';
        for (const Clip& c : t.clips) {
            out << std::setprecision(17) << c.start << ' ' << c.dur << ' ' << c.color << ' ' << c.id << ' ' << c.runs << ' ' << (c.muted ? 1 : 0) << '\n';
            writeString(out, c.name);
            writeString(out, c.code);
            writeString(out, c.note);
            writeString(out, std::to_string(c.lang));
        }
        out << t.midiClips.size() << '\n';
        for (const MidiClip& mc : t.midiClips) {
            out << mc.id << ' ' << mc.clipId << ' ' << std::setprecision(17) << mc.start << ' ' << mc.dur << ' ' << mc.notes.size() << '\n';
            for (const MidiNote& note : mc.notes)
                out << std::setprecision(17) << note.start << ' ' << note.dur << ' ' << note.pitch << ' ' << note.velocity << '\n';
        }
        out << t.automation.size() << '\n';
        for (const auto& [channel, points] : t.automation) {
            writeString(out, channel);
            out << points.size() << '\n';
            for (const AutoPoint& point : points) out << std::setprecision(17) << point.t << ' ' << point.v << '\n';
        }
        out << t.automationHeights.size() << '\n';
        for (const auto& [channel, height] : t.automationHeights) {
            writeString(out, channel);
            out << std::setprecision(9) << height << '\n';
        }
        out << t.automationColors.size() << '\n';
        for (const auto& [channel, color] : t.automationColors) {
            writeString(out, channel);
            out << color << '\n';
        }
        out << (t.midiHidden ? 1 : 0) << ' ' << t.hiddenAutomation.size() << '\n';
        for (const std::string& channel : t.hiddenAutomation) writeString(out, channel);
        out << "END_TRACK\n";
    }
    // version 13: score block
    const ScoreInfo& sc = p.score;
    out << "SCORE " << std::setprecision(17) << sc.sysSec << ' ' << sc.sections.size() << ' ' << sc.markers.size() << '\n';
    writeString(out, sc.title);
    writeString(out, sc.subtitle);
    writeString(out, sc.composer);
    for (const auto* list : { &sc.sections, &sc.markers })
        for (const ScoreMark& mk : *list) {
            out << std::setprecision(17) << mk.t << '\n';
            writeString(out, mk.label);
            writeString(out, mk.text);
        }
    for (const Track& t : p.tracks) {
        out << t.scoreKind << ' ' << t.scoreClef << '\n';
        writeString(out, t.scoreLabel);
        writeString(out, t.scoreSub);
    }
    // version 14: lanes, modes, per clip / per automation score options
    out << "SCORE2 " << sc.mode << ' ' << sc.systemsPerPage << ' ' << (sc.coverPage ? 1 : 0) << ' ' << (sc.eventList ? 1 : 0) << ' ' << sc.nextLaneId << ' ' << sc.lanes.size() << '\n';
    writeString(out, sc.notes);
    for (const ScoreLaneDef& l : sc.lanes) {
        out << l.id << ' ' << l.kind << ' ' << l.clef << ' ' << l.color << '\n';
        writeString(out, l.name);
        writeString(out, l.sub);
    }
    for (const Track& t : p.tracks) {
        out << t.scoreLane << ' ' << t.scoreAuto.size() << '\n';
        for (const auto& [channel, mode] : t.scoreAuto) { writeString(out, channel); out << mode << '\n'; }
        for (const Clip& c : t.clips) {
            out << c.scoreLane << " 0 " << c.scoreStyle << '\n';   // the 0 was a per-clip MIDI display, now a setting of the MIDI lane
            writeString(out, c.scoreLabel);
        }
    }
    // version 15: spectrum lane + frequency bands
    out << "SCORE3 " << (sc.spectrum ? 1 : 0) << '\n';
    for (const Track& t : p.tracks)
        for (const Clip& c : t.clips) out << std::setprecision(9) << c.scoreFreqLo << ' ' << c.scoreFreqHi << '\n';
    // version 16: merge by name, written notes and dynamics per clip (the 0 4 4 were tempo and meter, no longer used)
    out << "SCORE4 0 4 4 " << (sc.mergeSameName ? 1 : 0) << '\n';
    for (const Track& t : p.tracks)
        for (const Clip& c : t.clips) {
            writeString(out, c.scoreWritten);
            writeString(out, c.scoreDynamic);
        }
    // version 18: line weight, copyright, names and display of the MIDI / automation lanes, role and detail of the clips
    out << "SCORE6 " << std::setprecision(4) << sc.lineWeight << '\n';
    writeString(out, sc.copyright);
    for (const Track& t : p.tracks) {
        writeString(out, t.scoreMidiLabel);
        out << t.scoreMidiShow << ' ' << t.scoreAutoNames.size() << '\n';
        for (const auto& [channel, name] : t.scoreAutoNames) { writeString(out, channel); writeString(out, name); }
        for (const Clip& c : t.clips) out << c.scoreRole << ' ' << c.scoreDetail << ' ' << (c.scoreShowLang ? 1 : 0) << '\n';
    }
    // version 19: lane overlap / separated per track, analysis while rendering
    out << "SCORE7 " << (sc.analyze ? 1 : 0) << '\n';
    for (const Track& t : p.tracks) out << t.scoreOverlap << '\n';
    // version 20: track kinds (marker track, free automation), template language, automation line shapes in the score
    out << "TRACKS8\n";
    for (const Track& t : p.tracks) {
        out << t.kind << ' ' << t.lang << ' ' << std::setprecision(12) << t.autoMin << ' ' << t.autoMax << ' ' << t.marks.size() << ' ' << t.scoreAutoShape.size() << '\n';
        for (const TrackMark& m : t.marks) { out << std::setprecision(12) << m.t << ' ' << m.dur << ' ' << m.color << '\n'; writeString(out, m.text); }
        for (const auto& [channel, shape] : t.scoreAutoShape) {
            const auto cv = t.scoreAutoCurve.find(channel);
            writeString(out, channel);
            out << shape << ' ' << std::setprecision(6) << (cv == t.scoreAutoCurve.end() ? 1.0 : cv->second) << '\n';
        }
    }
    // version 21: graph lines: combine / half combine / separated, M and S
    out << "GRAPH1\n";
    for (const Track& t : p.tracks) {
        out << t.scoreAutoMerge.size() << ' ' << t.scoreAutoMuted.size() << ' ' << t.scoreAutoSolo.size() << '\n';
        for (const auto& [channel, m] : t.scoreAutoMerge) { writeString(out, channel); out << m << '\n'; }
        for (const std::string& c : t.scoreAutoMuted) writeString(out, c);
        for (const std::string& c : t.scoreAutoSolo) writeString(out, c);
    }
    // version 22: height of the graph lines in the score
    out << "GRAPH2\n";
    for (const Track& t : p.tracks) {
        out << t.scoreAutoYSize.size() << '\n';
        for (const auto& [channel, y] : t.scoreAutoYSize) { writeString(out, channel); out << y << '\n'; }
    }
    // version 23: stroke width of the automation / graph lines in the score
    out << "GRAPH3\n";
    for (const Track& t : p.tracks) {
        out << t.scoreAutoStroke.size() << '\n';
        for (const auto& [channel, w] : t.scoreAutoStroke) { writeString(out, channel); out << std::setprecision(6) << w << '\n'; }
    }
    // version 24: marker / graph track items: type, shape and where they show in the score
    out << "MARKS2\n";
    for (const Track& t : p.tracks) {
        out << t.marks.size() << '\n';
        for (const TrackMark& m : t.marks) out << m.type << ' ' << m.shape << ' ' << m.scorePos << '\n';
    }
    // version 25: code language of the marker items
    out << "MARKS3\n";
    for (const Track& t : p.tracks) {
        out << t.marks.size() << '\n';
        for (const TrackMark& m : t.marks) out << m.lang << '\n';
    }
    // version 26: size of the shape / box and font size of the marker and graph items
    out << "MARKS4\n";
    for (const Track& t : p.tracks) {
        out << t.marks.size() << '\n';
        for (const TrackMark& m : t.marks) out << m.size << ' ' << m.font << '\n';
    }
    // version 28: curve (ratio) of the swell / fade shapes
    out << "MARKS5\n";
    for (const Track& t : p.tracks) {
        out << t.marks.size() << '\n';
        for (const TrackMark& m : t.marks) out << m.ratio << '\n';
    }
    // version 29: border of the graph items in the score
    out << "MARKS6\n";
    for (const Track& t : p.tracks) {
        out << t.marks.size() << '\n';
        for (const TrackMark& m : t.marks) out << (m.border ? 1 : 0) << '\n';
    }
    // version 27: automation points inside a Pen 2 stroke (no dot)
    out << "AUTOLINE\n";
    for (const Track& t : p.tracks)
        for (const auto& lane : t.automation) {
            std::vector<size_t> idx;
            for (size_t i = 0; i < lane.second.size(); ++i) if (lane.second[i].line) idx.push_back(i);
            out << idx.size();
            for (size_t i : idx) out << ' ' << i;
            out << '\n';
        }
    // version 31: folder groups of the track panel
    out << "FOLDERS1\n" << p.folders.size() << '\n';
    for (const TrackFolder& f : p.folders) { out << f.id << ' ' << (f.collapsed ? 1 : 0) << ' ' << f.color << '\n'; writeString(out, f.name); }
    for (const Track& t : p.tracks) out << t.folderId << '\n';
    // version 33: project-wide UDO source and named audio-file references
    out << "INSERT1\n";
    writeString(out, p.udoCode);
    out << p.audioPool.size() << '\n';
    for (const AudioAsset& asset : p.audioPool) {
        writeString(out, asset.name);
        writeString(out, asset.path);
    }
    // version 34: project include files
    out << "POOL2\n" << p.includes.size() << '\n';
    for (const IncludeAsset& asset : p.includes) {
        writeString(out, asset.name);
        writeString(out, asset.path);
        out << (asset.score ? 1 : 0) << '\n';
    }
    // version 35: detailed-score annotation preference
    out << "SCORE8 " << (p.score.detailedDataLabels ? 1 : 0) << '\n';
    out << "INSTR1\n" << p.instrPool.size() << '\n';
    for (const std::string& source : p.instrPool) writeString(out, source);
    out << "CLIPUI1\n";
    for (const Track& t : p.tracks) {
        out << t.automationRanges.size() << '\n';
        for (const auto& entry : t.automationRanges) {
            writeString(out, entry.first);
            const CawParam& a = entry.second;
            out << std::setprecision(17) << a.min << ' ' << a.max << ' ' << a.step << ' ' << a.value << '\n';
        }
        out << (t.templateMidiEnabled ? 1 : 0) << '\n';
        writeString(out, t.templatePFieldRules);
        for (const Clip& c : t.clips) {
            out << (c.midiEnabled ? 1 : 0) << '\n';
            writeString(out, c.pFieldRules);
        }
    }
    out << "ORCHESTRA1\n";
    writeString(out, p.orchestraCode);
}

bool readMidiNote(std::istream& in, MidiNote& note) {
    if (!(in >> note.start >> note.dur >> note.pitch >> note.velocity) || note.start < 0.0 || note.dur <= 0.0) return false;
    note.pitch = std::min(127, std::max(0, note.pitch));
    note.velocity = std::min(127, std::max(1, note.velocity));
    return true;
}

// Import the former block into the new controls; never rewrite the source file
// on disk, only the in-memory project the user can choose to save.
void migrateOldCaw(Track& track, Clip& clip) {
    const size_t a = clip.code.find("<caw>"), b = clip.code.find("</caw>");
    if (a == std::string::npos || b == std::string::npos || b <= a) return;
    const std::string block = clip.code.substr(a + 5, b - a - 5);
    const ParsedTrack old = parseCode(clip.name, clip.code, false, clip.runs);
    clip.midiEnabled = old.midiEnabled;
    for (const CawParam& p : old.params)
        if (!track.automationRanges.count(p.chn)) track.automationRanges[p.chn] = p;
    static const std::regex pLine(R"re(\{\s*p\s*:\s*\d+\s*,[^}]*\})re");
    for (std::sregex_iterator it(block.begin(), block.end(), pLine), end; it != end; ++it)
        clip.pFieldRules += (clip.pFieldRules.empty() ? "" : "\n") + it->str();
    static const std::regex recRe(R"re(\{\s*rec\s*:\s*([A-Za-z_]\w*)\s*\})re");
    std::smatch rec;
    const bool hasRec = std::regex_search(block, rec, recRe);
    clip.code.erase(a, b + 6 - a);
    if (hasRec) {
        if (!clip.code.empty() && clip.code.back() != '\n') clip.code += '\n';
        clip.code += "chnset " + rec[1].str() + ", \"rec\"\n";
    }
}

// Projects from before version 4 had one code + one MIDI list per lane: give every clip its own copy.
void splitLegacyTrack(Track& t, std::vector<MidiNote>& laneNotes) {
    for (size_t k = 0; k < t.clips.size(); ++k) {
        Clip& c = t.clips[k];
        c.name = t.name + std::to_string(k + 1);
        c.code = t.code;
    }
    std::stable_sort(laneNotes.begin(), laneNotes.end(), [](const MidiNote& a, const MidiNote& b) { return a.start < b.start; });
    for (const MidiNote& note : laneNotes) {
        if (t.clips.empty()) break;
        Clip* owner = nullptr;
        for (Clip& c : t.clips)
            if (note.start >= c.start && note.start < c.start + c.dur) { owner = &c; break; }
        if (!owner) {
            for (Clip& c : t.clips)
                if (c.start <= note.start && (!owner || c.start > owner->start)) owner = &c;
        }
        if (!owner) owner = &t.clips.front();
        MidiNote relative = note;
        relative.start = std::max(0.0, note.start - owner->start);
        owner->legacyNotes.push_back(relative);
    }
}

// Old files stored the notes inside the Csound clip (and its MIDI cut points). Turn them into free MIDI clips.
void convertLegacyMidi(Track& t) {
    for (Clip& c : t.clips) {
        const bool usesMidi = parseClip(c).midiEnabled;
        if (c.legacyNotes.empty() && c.legacyCuts.empty() && !usesMidi) continue;
        std::vector<double> bounds{0.0};
        std::vector<double> cuts = c.legacyCuts;
        std::sort(cuts.begin(), cuts.end());
        for (double x : cuts) if (x > bounds.back() + 0.005 && x < c.dur - 0.005) bounds.push_back(x);
        bounds.push_back(c.dur);
        for (size_t k = 0; k + 1 < bounds.size(); ++k) {
            MidiClip mc;
            mc.clipId = c.id;
            mc.start = c.start + bounds[k];
            mc.dur = bounds[k + 1] - bounds[k];
            for (MidiNote n : c.legacyNotes) {
                const bool last = k + 2 == bounds.size();
                if (n.start >= bounds[k] - 1e-9 && (n.start < bounds[k + 1] - 1e-9 || last)) {
                    n.start -= bounds[k];
                    mc.notes.push_back(n);
                }
            }
            t.midiClips.push_back(std::move(mc));
        }
        c.legacyNotes.clear();
        c.legacyCuts.clear();
    }
}

bool readProject(std::istream& in, Project& project, std::string& error) {
    Project candidate;
    std::string magic;
    int version = 0;
    if (!(in >> magic >> version) || magic != "CAW_PROJECT" || (version < 1 || version > 38)) {
        error = "This is not a supported CAW project file.";
        return false;
    }
    CsoundSettings& s = candidate.cs;
    if (!(in >> s.sr >> s.ksmps >> s.nchnls >> s.nchnlsIn >> s.zdbfs >> s.a4 >> s.bufSW >> s.bufHW
          >> s.outDev >> s.inDev >> s.renderFormat >> s.tail >> s.editorFontSize >> s.editorFontStyle)) {
        error = "The CAW project settings are incomplete.";
        return false;
    }
    in.get();
    if (!readString(in, s.rtaudio) || !readString(in, s.csoundExe) || !readString(in, s.renderFile)) {
        error = "The CAW project settings are invalid.";
        return false;
    }
    if (version >= 10) {
        if (!readString(in, s.options)) { error = "The CAW project settings are invalid."; return false; }
    } else {
        s.options = defaultOptions(s);   // older projects: start from the options the old settings produced
    }
    { std::string unused; if (version >= 11 && !readString(in, unused)) { error = "The CAW project settings are invalid."; return false; }
    if (version >= 12 && !readString(in, unused)) { error = "The CAW project settings are invalid."; return false; } }
    if (version >= 30) {
        if (!(in >> s.syncMs)) { error = "The CAW project settings are invalid."; return false; }
        in.get();
        s.syncMs = std::min(1000, std::max(-500, s.syncMs));
    }
    size_t trackCount = 0;
    if (!(in >> trackCount) || trackCount > 10000) { error = "The track count is invalid."; return false; }
    in.get();
    for (size_t ti = 0; ti < trackCount; ++ti) {
        std::string marker;
        Track t;
        int muted = 0, solo = 0;
        if (!(in >> marker >> t.id >> muted >> solo >> t.height) || marker != "TRACK") {
            error = "A track entry is invalid."; return false;
        }
        if (version >= 2 && !(in >> t.color)) { error = "A track color is invalid."; return false; }
        in.get();
        t.muted = muted != 0;
        t.solo = solo != 0;
        t.height = std::min(320.f, std::max(44.f, t.height));
        if (!readString(in, t.name) || !readString(in, t.code)) { error = "A track's text is invalid."; return false; }
        size_t clipCount = 0;
        if (!(in >> clipCount) || clipCount > 1000000) { error = "A track's clip count is invalid."; return false; }
        for (size_t ci = 0; ci < clipCount; ++ci) {
            Clip c;
            if (!(in >> c.start >> c.dur) || c.start < 0.0 || c.dur <= 0.0) {
                error = "A clip entry is invalid."; return false;
            }
            if (version >= 4) {
                if (!(in >> c.color >> c.id)) { error = "A clip entry is invalid."; return false; }
                if (version >= 5) {
                    if (!(in >> c.runs)) { error = "A clip entry is invalid."; return false; }
                    c.runs = std::min(256, std::max(1, c.runs));
                }
                if (version >= 6) {
                    int mutedFlag = 0;
                    if (!(in >> mutedFlag)) { error = "A clip entry is invalid."; return false; }
                    c.muted = mutedFlag != 0;
                }
                in.get();
                if (!readString(in, c.name) || !readString(in, c.code)) { error = "A clip's text is invalid."; return false; }
                if (version >= 6 && !readString(in, c.note)) { error = "A clip's text is invalid."; return false; }
                if (version >= 11) {
                    std::string langText;
                    if (!readString(in, langText)) { error = "A clip's text is invalid."; return false; }
                    c.lang = 0;   // old project files may hold Pure Data / SuperCollider clips: they are Csound clips now (the code is kept as text)
                }
                if (version < 9) {   // old files kept the notes inside the Csound clip
                    size_t noteCount = 0;
                    if (!(in >> noteCount) || noteCount > 1000000) { error = "A MIDI note count is invalid."; return false; }
                    for (size_t ni = 0; ni < noteCount; ++ni) {
                        MidiNote note;
                        if (!readMidiNote(in, note)) { error = "A MIDI note is invalid."; return false; }
                        c.legacyNotes.push_back(note);
                    }
                    if (version >= 8) {
                        size_t cutCount = 0;
                        if (!(in >> cutCount) || cutCount > 100000) { error = "A MIDI cut count is invalid."; return false; }
                        for (size_t k = 0; k < cutCount; ++k) {
                            double cut = 0.0;
                            if (!(in >> cut)) { error = "A MIDI cut is invalid."; return false; }
                            c.legacyCuts.push_back(cut);
                        }
                    }
                }
            }
            t.clips.push_back(std::move(c));
        }
        if (version >= 9) {
            size_t midiClipCount = 0;
            if (!(in >> midiClipCount) || midiClipCount > 1000000) { error = "A track's MIDI clip count is invalid."; return false; }
            for (size_t mi = 0; mi < midiClipCount; ++mi) {
                MidiClip mc;
                size_t noteCount = 0;
                if (!(in >> mc.id >> mc.clipId >> mc.start >> mc.dur >> noteCount) || mc.dur <= 0.0 || noteCount > 1000000) {
                    error = "A MIDI clip entry is invalid."; return false;
                }
                for (size_t ni = 0; ni < noteCount; ++ni) {
                    MidiNote note;
                    if (!readMidiNote(in, note)) { error = "A MIDI note is invalid."; return false; }
                    mc.notes.push_back(note);
                }
                t.midiClips.push_back(std::move(mc));
            }
        }
        std::vector<MidiNote> legacyNotes;
        size_t channelCount = 0;
        if (!(in >> channelCount) || channelCount > 10000) { error = "An automation channel count is invalid."; return false; }
        for (size_t ai = 0; ai < channelCount; ++ai) {
            std::string channel;
            if (!readString(in, channel)) { error = "An automation channel is invalid."; return false; }
            size_t pointCount = 0;
            if (!(in >> pointCount) || pointCount > 1000000) { error = "An automation point count is invalid."; return false; }
            std::vector<AutoPoint> points;
            points.reserve(pointCount);
            for (size_t pi = 0; pi < pointCount; ++pi) {
                AutoPoint point{};
                if (!(in >> point.t >> point.v) || point.t < 0.0) { error = "An automation point is invalid."; return false; }
                points.push_back(point);
            }
            std::stable_sort(points.begin(), points.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.t < b.t; });
            t.automation[channel] = std::move(points);
        }
        if (version >= 2) {
            size_t heightCount = 0;
            if (!(in >> heightCount) || heightCount > 10000) { error = "An automation lane height count is invalid."; return false; }
            for (size_t hi = 0; hi < heightCount; ++hi) {
                std::string channel;
                float height = 0.f;
                if (!readString(in, channel) || !(in >> height)) { error = "An automation lane height is invalid."; return false; }
                in.get();
                t.automationHeights[channel] = std::min(320.f, std::max(28.f, height));
            }
            if (version >= 3) {
                size_t colorCount = 0;
                if (!(in >> colorCount) || colorCount > 10000) { error = "An automation lane color count is invalid."; return false; }
                for (size_t ci = 0; ci < colorCount; ++ci) {
                    std::string channel;
                    uint32_t color = 0;
                    if (!readString(in, channel) || !(in >> color)) { error = "An automation lane color is invalid."; return false; }
                    in.get();
                    t.automationColors[channel] = color;
                }
            }
            if (version < 4) {
                size_t noteCount = 0;
                if (!(in >> noteCount) || noteCount > 1000000) { error = "A MIDI note count is invalid."; return false; }
                for (size_t ni = 0; ni < noteCount; ++ni) {
                    MidiNote note;
                    if (!readMidiNote(in, note)) { error = "A MIDI note is invalid."; return false; }
                    legacyNotes.push_back(note);
                }
            }
        }
        if (version < 4) splitLegacyTrack(t, legacyNotes);
        if (version < 9) convertLegacyMidi(t);
        if (version >= 7) {
            int hiddenMidi = 0;
            size_t hiddenCount = 0;
            if (!(in >> hiddenMidi >> hiddenCount) || hiddenCount > 10000) { error = "Hidden lane data is invalid."; return false; }
            t.midiHidden = hiddenMidi != 0;
            for (size_t hi = 0; hi < hiddenCount; ++hi) {
                std::string channel;
                if (!readString(in, channel)) { error = "Hidden lane data is invalid."; return false; }
                t.hiddenAutomation.insert(channel);
            }
        }
        if (!(in >> marker) || marker != "END_TRACK") { error = "A track is not terminated correctly."; return false; }
        in.get();
        for (const auto& lane : t.automation) {
            auto height = t.automationHeights.find(lane.first);
            if (height == t.automationHeights.end()) t.automationHeights[lane.first] = 42.f;
        }
        t.editorOpen = false;
        t.deleteSelected = false;
        candidate.tracks.push_back(std::move(t));
    }
    if (version >= 13) {
        std::string marker;
        size_t secCount = 0, markCount = 0;
        ScoreInfo& sc = candidate.score;
        if (!(in >> marker >> sc.sysSec >> secCount >> markCount) || marker != "SCORE" || secCount > 100000 || markCount > 100000) {
            error = "The score block is invalid."; return false;
        }
        if (!readString(in, sc.title) || !readString(in, sc.subtitle) || !readString(in, sc.composer)) { error = "The score block is invalid."; return false; }
        sc.sysSec = std::min(600.0, std::max(20.0, sc.sysSec));
        for (size_t i = 0; i < secCount + markCount; ++i) {
            ScoreMark mk;
            if (!(in >> mk.t) || !readString(in, mk.label) || !readString(in, mk.text)) { error = "A score mark is invalid."; return false; }
            (i < secCount ? sc.sections : sc.markers).push_back(std::move(mk));
        }
        for (Track& t : candidate.tracks) {
            if (!(in >> t.scoreKind >> t.scoreClef) || !readString(in, t.scoreLabel) || !readString(in, t.scoreSub)) { error = "A score lane entry is invalid."; return false; }
            t.scoreKind = std::min(3, std::max(0, t.scoreKind));
            t.scoreClef = std::min(6, std::max(0, t.scoreClef));
        }
    }
    if (version >= 14) {
        std::string marker;
        ScoreInfo& sc = candidate.score;
        size_t laneCount = 0;
        int cover = 1, events = 1;
        if (!(in >> marker >> sc.mode >> sc.systemsPerPage >> cover >> events >> sc.nextLaneId >> laneCount) || marker != "SCORE2" || laneCount > 10000) {
            error = "The score block (2) is invalid."; return false;
        }
        sc.coverPage = cover != 0; sc.eventList = events != 0;
        sc.mode = std::min(1, std::max(0, sc.mode));
        sc.systemsPerPage = std::min(12, std::max(0, sc.systemsPerPage));
        if (!readString(in, sc.notes)) { error = "The score notes are invalid."; return false; }
        for (size_t i = 0; i < laneCount; ++i) {
            ScoreLaneDef l;
            if (!(in >> l.id >> l.kind >> l.clef >> l.color) || !readString(in, l.name) || !readString(in, l.sub)) { error = "A score lane is invalid."; return false; }
            l.kind = std::min(2, std::max(0, l.kind));
            l.clef = std::min(6, std::max(0, l.clef));
            sc.lanes.push_back(std::move(l));
        }
        for (Track& t : candidate.tracks) {
            size_t autoCount = 0;
            if (!(in >> t.scoreLane >> autoCount) || autoCount > 10000) { error = "A track score entry is invalid."; return false; }
            for (size_t i = 0; i < autoCount; ++i) {
                std::string channel; int mode = 1;
                if (!readString(in, channel) || !(in >> mode)) { error = "A score automation entry is invalid."; return false; }
                t.scoreAuto[channel] = std::min(2, std::max(0, mode));
            }
            for (Clip& c : t.clips) {
                int legacyMidi = 0;
                if (!(in >> c.scoreLane >> legacyMidi >> c.scoreStyle) || !readString(in, c.scoreLabel)) { error = "A clip score entry is invalid."; return false; }
                c.scoreStyle = std::min(6, std::max(0, c.scoreStyle));
            }
        }
    }
    if (version >= 15) {
        std::string marker;
        int spec = 0;
        if (!(in >> marker >> spec) || marker != "SCORE3") { error = "The score block (3) is invalid."; return false; }
        candidate.score.spectrum = spec != 0;
        for (Track& t : candidate.tracks)
            for (Clip& c : t.clips) {
                if (!(in >> c.scoreFreqLo >> c.scoreFreqHi)) { error = "A clip frequency band is invalid."; return false; }
                c.scoreFreqLo = std::max(0.0, c.scoreFreqLo);
                c.scoreFreqHi = std::max(0.0, c.scoreFreqHi);
            }
    }
    if (version >= 16) {
        std::string marker;
        int merge = 1;
        ScoreInfo& sc = candidate.score;
        double oldTempo; int oldNum, oldDen;   // tempo and meter were dropped from the format
        if (!(in >> marker >> oldTempo >> oldNum >> oldDen >> merge) || marker != "SCORE4") { error = "The score block (4) is invalid."; return false; }
        sc.mergeSameName = merge != 0;
        for (Track& t : candidate.tracks)
            for (Clip& c : t.clips)
                if (!readString(in, c.scoreWritten) || !readString(in, c.scoreDynamic)) { error = "A clip's written notes are invalid."; return false; }
    }
    if (version == 17) {   // line weight, then two names per track (only the MIDI name is still used)
        std::string marker, ignored;
        ScoreInfo& sc = candidate.score;
        if (!(in >> marker >> sc.lineWeight) || marker != "SCORE5") { error = "The score block (5) is invalid."; return false; }
        sc.lineWeight = std::min(3.0, std::max(0.4, sc.lineWeight));
        for (Track& t : candidate.tracks)
            if (!readString(in, t.scoreMidiLabel) || !readString(in, ignored)) { error = "A track's score names are invalid."; return false; }
    }
    if (version >= 18) {
        std::string marker;
        ScoreInfo& sc = candidate.score;
        if (!(in >> marker >> sc.lineWeight) || marker != "SCORE6" || !readString(in, sc.copyright)) { error = "The score block (6) is invalid."; return false; }
        sc.lineWeight = std::min(3.0, std::max(0.4, sc.lineWeight));
        for (Track& t : candidate.tracks) {
            size_t nameCount = 0;
            if (!readString(in, t.scoreMidiLabel) || !(in >> t.scoreMidiShow >> nameCount) || nameCount > 10000) { error = "A track's score names are invalid."; return false; }
            t.scoreMidiShow = std::min(5, std::max(0, t.scoreMidiShow));
            for (size_t i = 0; i < nameCount; ++i) {
                std::string channel, name;
                if (!readString(in, channel) || !readString(in, name)) { error = "A score automation name is invalid."; return false; }
                t.scoreAutoNames[channel] = name;
            }
            for (Clip& c : t.clips) {
                int showLang = 0;
                if (!(in >> c.scoreRole >> c.scoreDetail >> showLang)) { error = "A clip score entry is invalid."; return false; }
                c.scoreRole = std::min(2, std::max(0, c.scoreRole));
                c.scoreDetail = std::min(1, std::max(0, c.scoreDetail));
                c.scoreShowLang = showLang != 0;
            }
        }
    }
    if (version >= 19) {
        std::string marker;
        int an = 1;
        if (!(in >> marker >> an) || marker != "SCORE7") { error = "The score block (7) is invalid."; return false; }
        candidate.score.analyze = an != 0;
        for (Track& t : candidate.tracks) {
            if (!(in >> t.scoreOverlap)) { error = "A track's score overlap setting is invalid."; return false; }
            t.scoreOverlap = std::min(1, std::max(0, t.scoreOverlap));
        }
    }
    if (version >= 20) {
        std::string marker;
        if (!(in >> marker) || marker != "TRACKS8") { error = "The track block (8) is invalid."; return false; }
        for (Track& t : candidate.tracks) {
            size_t markCount = 0, shapeCount = 0;
            if (!(in >> t.kind >> t.lang >> t.autoMin >> t.autoMax >> markCount >> shapeCount) || markCount > 100000 || shapeCount > 10000) { error = "A track's kind is invalid."; return false; }
            t.kind = std::min(2, std::max(0, t.kind));
            t.lang = 0;
            if (!(t.autoMax > t.autoMin)) t.autoMax = t.autoMin + 1.0;
            for (size_t i = 0; i < markCount; ++i) {
                TrackMark m;
                if (!(in >> m.t >> m.dur >> m.color) || !readString(in, m.text)) { error = "A marker is invalid."; return false; }
                m.dur = std::max(0.0, m.dur);
                t.marks.push_back(m);
            }
            for (size_t i = 0; i < shapeCount; ++i) {
                std::string channel; int shape = 0; double curve = 1.0;
                if (!readString(in, channel) || !(in >> shape >> curve)) { error = "An automation shape is invalid."; return false; }
                t.scoreAutoShape[channel] = std::min(9, std::max(0, shape));
                t.scoreAutoCurve[channel] = std::min(3.0, std::max(0.3, curve));
            }
        }
    }
    if (version >= 21) {
        std::string marker;
        if (!(in >> marker) || marker != "GRAPH1") { error = "The graph block is invalid."; return false; }
        for (Track& t : candidate.tracks) {
            size_t nm = 0, nmu = 0, nso = 0;
            if (!(in >> nm >> nmu >> nso) || nm > 10000 || nmu > 10000 || nso > 10000) { error = "A graph entry is invalid."; return false; }
            for (size_t i = 0; i < nm; ++i) {
                std::string channel; int m = 0;
                if (!readString(in, channel) || !(in >> m)) { error = "A graph line mode is invalid."; return false; }
                t.scoreAutoMerge[channel] = std::min(2, std::max(0, m));
            }
            for (size_t i = 0; i < nmu; ++i) { std::string c; if (!readString(in, c)) { error = "A graph mute is invalid."; return false; } t.scoreAutoMuted.insert(c); }
            for (size_t i = 0; i < nso; ++i) { std::string c; if (!readString(in, c)) { error = "A graph solo is invalid."; return false; } t.scoreAutoSolo.insert(c); }
        }
    }
    if (version >= 22) {
        std::string marker;
        if (!(in >> marker) || marker != "GRAPH2") { error = "The graph block (2) is invalid."; return false; }
        for (Track& t : candidate.tracks) {
            size_t n = 0;
            if (!(in >> n) || n > 10000) { error = "A graph height entry is invalid."; return false; }
            for (size_t i = 0; i < n; ++i) {
                std::string channel; int y = 100;
                if (!readString(in, channel) || !(in >> y)) { error = "A graph height is invalid."; return false; }
                t.scoreAutoYSize[channel] = std::min(400, std::max(50, y));
            }
        }
    }
    if (version >= 23) {
        std::string marker;
        if (!(in >> marker) || marker != "GRAPH3") { error = "The graph block (3) is invalid."; return false; }
        for (Track& t : candidate.tracks) {
            size_t n = 0;
            if (!(in >> n) || n > 10000) { error = "A graph stroke entry is invalid."; return false; }
            for (size_t i = 0; i < n; ++i) {
                std::string channel; double w = 0.0;
                if (!readString(in, channel) || !(in >> w)) { error = "A graph stroke width is invalid."; return false; }
                t.scoreAutoStroke[channel] = std::min(8.0, std::max(0.3, w));
            }
        }
    }
    if (version >= 24) {
        std::string marker;
        if (!(in >> marker) || marker != "MARKS2") { error = "The marker block (2) is invalid."; return false; }
        for (Track& t : candidate.tracks) {
            size_t n = 0;
            if (!(in >> n) || n != t.marks.size()) { error = "A marker entry is invalid."; return false; }
            for (TrackMark& m : t.marks) {
                if (!(in >> m.type >> m.shape >> m.scorePos)) { error = "A marker setting is invalid."; return false; }
                m.type = std::min(4, std::max(0, m.type));
                m.shape = std::min(23, std::max(0, m.shape));
                m.scorePos = std::min(2, std::max(0, m.scorePos));
            }
        }
    }
    if (version >= 25) {
        std::string marker;
        if (!(in >> marker) || marker != "MARKS3") { error = "The marker block (3) is invalid."; return false; }
        for (Track& t : candidate.tracks) {
            size_t n = 0;
            if (!(in >> n) || n != t.marks.size()) { error = "A marker entry is invalid."; return false; }
            for (TrackMark& m : t.marks) {
                if (!(in >> m.lang)) { error = "A marker language is invalid."; return false; }
                m.lang = 0;
            }
        }
    }
    if (version >= 26) {
        std::string marker;
        if (!(in >> marker) || marker != "MARKS4") { error = "The marker block (4) is invalid."; return false; }
        for (Track& t : candidate.tracks) {
            size_t n = 0;
            if (!(in >> n) || n != t.marks.size()) { error = "A marker entry is invalid."; return false; }
            for (TrackMark& m : t.marks) {
                if (!(in >> m.size >> m.font)) { error = "A marker size is invalid."; return false; }
                m.size = std::min(400, std::max(40, m.size));
                m.font = m.font <= 0 ? 0 : std::min(40, std::max(6, m.font));
                m.type = std::min(4, std::max(0, m.type));
            }
        }
    } else {
        // older files: the Icon type is gone (it becomes a comment), Code moves from 4 to 3, the old shapes (squares, music notes...) are replaced by graphic-score shapes
        for (Track& t : candidate.tracks)
            for (TrackMark& m : t.marks) {
                if (t.kind == 2) m.shape = 0;
                else if (m.type == 3) { m.type = 0; m.shape = 0; }
                else if (m.type == 4) m.type = 3;
            }
    }
    if (version >= 28) {
        std::string marker;
        if (!(in >> marker) || marker != "MARKS5") { error = "The marker block (5) is invalid."; return false; }
        for (Track& t : candidate.tracks) {
            size_t n = 0;
            if (!(in >> n) || n != t.marks.size()) { error = "A marker entry is invalid."; return false; }
            for (TrackMark& m : t.marks) {
                if (!(in >> m.ratio)) { error = "A marker ratio is invalid."; return false; }
                m.ratio = std::min(300, std::max(30, m.ratio));
            }
        }
    }
    if (version >= 29) {
        std::string marker;
        if (!(in >> marker) || marker != "MARKS6") { error = "The marker block (6) is invalid."; return false; }
        for (Track& t : candidate.tracks) {
            size_t n = 0;
            if (!(in >> n) || n != t.marks.size()) { error = "A marker entry is invalid."; return false; }
            for (TrackMark& m : t.marks) {
                int b = 0;
                if (!(in >> b)) { error = "A marker border is invalid."; return false; }
                m.border = b != 0;
            }
        }
    }
    if (version >= 27) {
        std::string marker;
        if (!(in >> marker) || marker != "AUTOLINE") { error = "The automation block is invalid."; return false; }
        for (Track& t : candidate.tracks)
            for (auto& lane : t.automation) {
                size_t n = 0;
                if (!(in >> n) || n > lane.second.size()) { error = "An automation entry is invalid."; return false; }
                for (size_t k = 0; k < n; ++k) {
                    size_t idx = 0;
                    if (!(in >> idx) || idx >= lane.second.size()) { error = "An automation entry is invalid."; return false; }
                    lane.second[idx].line = true;
                }
            }
    }
    if (version >= 31) {
        std::string marker;
        size_t nf = 0;
        if (!(in >> marker) || marker != "FOLDERS1" || !(in >> nf) || nf > 100000) { error = "The folder block is invalid."; return false; }
        for (size_t i = 0; i < nf; ++i) {
            TrackFolder f; int collapsed = 0;
            if (!(in >> f.id >> collapsed) || (version >= 32 && !(in >> f.color)) || !readString(in, f.name)) { error = "A folder is invalid."; return false; }
            f.collapsed = collapsed != 0;
            candidate.folders.push_back(f);
        }
        for (Track& t : candidate.tracks) {
            if (!(in >> t.folderId)) { error = "A track folder is invalid."; return false; }
            bool known = false;
            for (const TrackFolder& f : candidate.folders) if (f.id == t.folderId) known = true;
            if (!known) t.folderId = 0;
        }
    }
    if (version >= 33) {
        std::string marker;
        size_t n = 0;
        if (!(in >> marker) || marker != "INSERT1" || !readString(in, candidate.udoCode) ||
            !(in >> n) || n > 100000) {
            error = "The Insert pools are invalid.";
            return false;
        }
        for (size_t i = 0; i < n; ++i) {
            AudioAsset asset;
            if (!readString(in, asset.name) || !readString(in, asset.path)) {
                error = "An Audio file entry is invalid.";
                return false;
            }
            candidate.audioPool.push_back(std::move(asset));
        }
    }
    if (version >= 34) {
        std::string marker;
        size_t n = 0;
        if (!(in >> marker >> n) || marker != "POOL2" || n > 100000) {
            error = "The include files are invalid.";
            return false;
        }
        for (size_t i = 0; i < n; ++i) {
            IncludeAsset asset;
            int score = 0;
            if (!readString(in, asset.name) || !readString(in, asset.path) || !(in >> score)) {
                error = "An include file entry is invalid.";
                return false;
            }
            in.get();
            asset.score = score != 0;
            candidate.includes.push_back(std::move(asset));
        }
    }
    if (version >= 35) {
        int dataLabels = 0;
        std::string marker;
        if (!(in >> marker >> dataLabels) || marker != "SCORE8") {
            error = "The detailed-score settings are invalid.";
            return false;
        }
        candidate.score.detailedDataLabels = dataLabels != 0;
    }
    if (version >= 36) {
        std::string marker;
        size_t count = 0;
        if (!(in >> marker >> count) || marker != "INSTR1" || count > 100000) {
            error = "The instrument pool is invalid.";
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            std::string source;
            if (!readString(in, source)) { error = "An instrument entry is invalid."; return false; }
            candidate.instrPool.push_back(std::move(source));
        }
    }
    if (version >= 37) {
        std::string marker;
        if (!(in >> marker) || marker != "CLIPUI1") { error = "The clip controls are invalid."; return false; }
        for (Track& track : candidate.tracks) {
            size_t n = 0;
            if (!(in >> n) || n > 10000) { error = "The automation ranges are invalid."; return false; }
            for (size_t i = 0; i < n; ++i) {
                CawParam p;
                if (!readString(in, p.chn) || !(in >> p.min >> p.max >> p.step >> p.value) ||
                    !(p.max > p.min) || !(p.step > 0)) { error = "An automation range is invalid."; return false; }
                track.automationRanges[p.chn] = p;
            }
            int templateMidi = 0;
            if (!(in >> templateMidi) || !readString(in, track.templatePFieldRules)) { error = "The track template controls are invalid."; return false; }
            track.templateMidiEnabled = templateMidi != 0;
            for (Clip& clip : track.clips) {
                int midi = 0;
                if (!(in >> midi) || !readString(in, clip.pFieldRules)) { error = "A clip control is invalid."; return false; }
                clip.midiEnabled = midi != 0;
            }
        }
    }
    if (version >= 38) {
        std::string marker;
        if (!(in >> marker) || marker != "ORCHESTRA1" || !readString(in, candidate.orchestraCode)) {
            error = "The orchestra source is invalid."; return false;
        }
    }
    if (version < 37) {
        for (Track& track : candidate.tracks) {
            for (Clip& clip : track.clips) migrateOldCaw(track, clip);
            Clip tmpl; tmpl.name = track.name; tmpl.code = track.code;
            migrateOldCaw(track, tmpl);
            track.code = tmpl.code;
            track.templateMidiEnabled = tmpl.midiEnabled;
            track.templatePFieldRules = tmpl.pFieldRules;
        }
    }
    {
        int maxMidiId = 0;
        for (const Track& tr : candidate.tracks) for (const MidiClip& mc : tr.midiClips) maxMidiId = std::max(maxMidiId, mc.id);
        for (Track& tr : candidate.tracks) for (MidiClip& mc : tr.midiClips) if (mc.id <= 0) mc.id = ++maxMidiId;
    }
    s.sr = std::min(384000, std::max(8000, s.sr));
    s.ksmps = std::min(4096, std::max(1, s.ksmps));
    s.nchnls = std::min(64, std::max(1, s.nchnls));
    s.nchnlsIn = std::min(64, std::max(0, s.nchnlsIn));
    s.editorFontSize = std::min(36.f, std::max(10.f, s.editorFontSize));
    s.editorFontStyle = std::min(2, std::max(0, s.editorFontStyle));
    project = std::move(candidate);
    return true;
}
}  // namespace

bool saveProjectFile(const Project& p, const std::string& path, std::string& error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { error = "Could not open the project file for writing."; return false; }
    writeProject(out, p);
    out.flush();
    if (!out) { error = "An error occurred while writing the project file."; return false; }
    return true;
}

bool loadProjectFile(Project& project, const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "Could not open the project file."; return false; }
    return readProject(in, project, error);
}

std::string serializeProject(const Project& project) {
    std::ostringstream out(std::ios::binary);
    writeProject(out, project);
    return out.str();
}

bool deserializeProject(Project& project, const std::string& data, std::string& error) {
    std::istringstream in(data, std::ios::binary);
    return readProject(in, project, error);
}
