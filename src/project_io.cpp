#include "project_io.h"
#include "csd_builder.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
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
    out << "CAW_PROJECT 16\n"
        << s.sr << ' ' << s.ksmps << ' ' << s.nchnls << ' ' << s.nchnlsIn << ' '
        << std::setprecision(17) << s.zdbfs << ' ' << s.a4 << ' ' << s.bufSW << ' ' << s.bufHW << ' '
        << s.outDev << ' ' << s.inDev << ' ' << s.renderFormat << ' ' << s.tail << ' '
        << s.editorFontSize << ' ' << s.editorFontStyle << '\n';
    writeString(out, s.rtaudio);
    writeString(out, s.csoundExe);
    writeString(out, s.renderFile);
    writeString(out, s.options);
    writeString(out, s.pdExe);
    writeString(out, s.scExe);
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
            out << c.scoreLane << ' ' << c.scoreMidi << ' ' << c.scoreStyle << '\n';
            writeString(out, c.scoreLabel);
        }
    }
    // version 15: spectrum lane + frequency bands
    out << "SCORE3 " << (sc.spectrum ? 1 : 0) << '\n';
    for (const Track& t : p.tracks)
        for (const Clip& c : t.clips) out << std::setprecision(9) << c.scoreFreqLo << ' ' << c.scoreFreqHi << '\n';
    // version 16: bars (tempo, meter), merge by name, written notes and dynamics per clip
    out << "SCORE4 " << std::setprecision(9) << sc.tempo << ' ' << sc.meterNum << ' ' << sc.meterDen << ' ' << (sc.mergeSameName ? 1 : 0) << '\n';
    for (const Track& t : p.tracks)
        for (const Clip& c : t.clips) {
            writeString(out, c.scoreWritten);
            writeString(out, c.scoreDynamic);
        }
}

bool readMidiNote(std::istream& in, MidiNote& note) {
    if (!(in >> note.start >> note.dur >> note.pitch >> note.velocity) || note.start < 0.0 || note.dur <= 0.0) return false;
    note.pitch = std::min(127, std::max(0, note.pitch));
    note.velocity = std::min(127, std::max(1, note.velocity));
    return true;
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
    if (!(in >> magic >> version) || magic != "CAW_PROJECT" || (version < 1 || version > 16)) {
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
    if (version >= 11 && !readString(in, s.pdExe)) { error = "The CAW project settings are invalid."; return false; }
    if (version >= 12 && !readString(in, s.scExe)) { error = "The CAW project settings are invalid."; return false; }
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
                    c.lang = std::min(2, std::max(0, std::atoi(langText.c_str())));
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
            t.scoreClef = std::min(3, std::max(0, t.scoreClef));
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
            l.clef = std::min(3, std::max(0, l.clef));
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
                if (!(in >> c.scoreLane >> c.scoreMidi >> c.scoreStyle) || !readString(in, c.scoreLabel)) { error = "A clip score entry is invalid."; return false; }
                c.scoreMidi = std::min(4, std::max(0, c.scoreMidi));
                c.scoreStyle = std::min(3, std::max(0, c.scoreStyle));
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
        if (!(in >> marker >> sc.tempo >> sc.meterNum >> sc.meterDen >> merge) || marker != "SCORE4") { error = "The score block (4) is invalid."; return false; }
        sc.mergeSameName = merge != 0;
        sc.tempo = (sc.tempo > 0.0) ? std::min(400.0, std::max(20.0, sc.tempo)) : 0.0;
        sc.meterNum = std::min(32, std::max(1, sc.meterNum));
        sc.meterDen = (sc.meterDen == 2 || sc.meterDen == 4 || sc.meterDen == 8 || sc.meterDen == 16) ? sc.meterDen : 4;
        for (Track& t : candidate.tracks)
            for (Clip& c : t.clips)
                if (!readString(in, c.scoreWritten) || !readString(in, c.scoreDynamic)) { error = "A clip's written notes are invalid."; return false; }
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
