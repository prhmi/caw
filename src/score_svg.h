#pragma once
#include "model.h"
#include <cstdint>
#include <string>
#include <vector>

// Contemporary-music score (SVG export + live preview). The renderer draws into a list of primitives (ScorePage);
// the same list is written as SVG or drawn by the Caw preview window.
//  overview : the whole piece on ONE page.
//  detailed : the FULL SCORE, a multi-page document (A4 landscape): cover page, then systems that read from left to right and
//             top to bottom, page after page (barlines and bar numbers when a tempo is set), then cue-list pages.
//             Clips get cue numbers, times, parameters; automation lanes, spectrum and notes are shown.
struct ScorePrim {
    enum Kind { Line, Rect, Polygon, Polyline, Ellipse, Circle, Text } kind = Line;
    uint32_t stroke = 0;          // 0xRRGGBBAA, alpha 0 = none
    uint32_t fill = 0;
    float sw = 1.f;
    float dash0 = 0.f, dash1 = 0.f;
    float x = 0, y = 0, w = 0, h = 0;      // line: x,y -> w,h as x2,y2; rect: x,y,w,h; circle/ellipse: center x,y radius w (h = ry)
    float size = 10.f;                      // text size
    float angle = 0.f;                      // ellipse rotation (degrees)
    bool bold = false, italic = false, round = false;
    int anchor = 0;                         // text: 0 start, 1 middle, 2 end
    std::vector<float> pts;                 // polygon / polyline x,y pairs
    std::string text;
};

struct ScorePage {
    double w = 1500, h = 1060;
    std::vector<ScorePrim> prims;
};

// Spectrogram of the rendered audio file: `bands` log-spaced bands between fLo and fHi, `colsPerSec` columns per second.
struct ScoreSpectrum {
    int bands = 0;
    double colsPerSec = 0.0, fLo = 40.0, fHi = 16000.0, duration = 0.0;
    std::vector<uint8_t> data;              // [column * bands + band], 0..255
    bool valid() const { return bands > 0 && !data.empty(); }
};
bool analyzeScoreWav(const std::string& path, ScoreSpectrum& out, std::string& error);

struct ScoreRenderOptions {
    bool detailed = false;
    bool blackWhite = false;
    const ScoreSpectrum* spectrum = nullptr;   // used when Project::score.spectrum is on
};

// All pages of the score (overview = 1 page).
bool buildScorePages(const Project& project, const std::string& fallbackTitle, const ScoreRenderOptions& options,
                     std::vector<ScorePage>& pages, std::string& error);
std::string scorePageToSvg(const ScorePage& page);

// Writes the pages as SVG files (+ one .html with all pages inside, to view and print as PDF, for the full score).
// basePath: path without extension. `files`: written files, the first one is what to open.
bool writeScoreDocument(const Project& project, const std::string& fallbackTitle, const ScoreRenderOptions& options,
                        const std::string& basePath, std::vector<std::string>& files, std::string& error);

// What the score will contain, lane by lane (for the editor): tracks with the same name are one lane.
struct ScoreLaneInfo {
    std::string name;
    std::vector<std::string> tracks;       // track names (a name twice = two tracks put together)
    std::vector<std::string> automation;   // automation lanes shown under it
    bool staff = false, merged = false, noClef = false;
    int clips = 0, notes = 0;
};
std::vector<ScoreLaneInfo> scoreLaneSummary(const Project& project);

// "C4 E4:2 - G3+D4": notes written by hand (name[:relative length], "-" = rest, "+" = chord), spread over `dur` seconds.
std::vector<MidiNote> scoreWrittenNotes(const std::string& text, double dur);
