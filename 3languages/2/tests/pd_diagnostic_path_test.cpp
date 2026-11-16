#include "../src/pd_host_mixer.h"
#include "../src/pd_diagnostics.h"

#include <cassert>
#include <iostream>
#include <regex>
#include <sstream>

// End-to-end test of the original bug: the actual PdBackend print hook,
// host-mixer notes, newline-delimited log transport, console clip regex,
// and readable-editor line matcher.
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    pd_timeline::Event event{};
    event.clipId = 42;
    event.trackId = 1;
    event.start = 0.1;
    event.end = 3.0;
    event.patch = argv[2];
    event.runs = 1;

    PdHostMixer mixer;
    std::string error;
    assert(mixer.prepare(argv[1], {event}, 48000, 2, 64, error));
    const float* audio = nullptr;
    // Diagnostics from preload should be available even before Play.
    assert(mixer.render(64, audio, error));
    const auto messages = mixer.takeRuntimeNotes();
    assert(messages.size() == 1);
    assert(messages[0].find("PD clip 42:") == 0);
    assert(messages[0].find('\n') == std::string::npos);

    // Same wire and regex as CsoundApiRunner::LogQueue + logCsoundLine.
    const std::string wire = "[caw] " + messages[0] + "\n";
    std::istringstream transport(wire);
    std::string row;
    assert(static_cast<bool>(std::getline(transport, row)));
    const std::regex clipRe(R"re(PD clip\s+(\d+):\s*(.*))re");
    std::smatch match;
    assert(std::regex_search(row, match, clipRe));
    assert(match[1].str() == "42");
    const auto loc = pd_diagnostics::inferEditorLine(
        "oc~ 440;\n*~ 0.08;\ndac~ 1 2;\nCAW test tone;\n",
        match[2].str(), {1, 2, 3});
    assert(loc.line == 1);
    assert(loc.candidates.empty());
    assert(!loc.approximate);
    assert(!std::getline(transport, row));

    // Audition opens a fresh libpd instance and must carry the same context.
    mixer.audition(event, true);
    assert(mixer.render(64, audio, error));
    const auto auditionMessages = mixer.takeRuntimeNotes();
    assert(auditionMessages.size() == 1);
    assert(auditionMessages[0].find("PD clip 42:") == 0);
    assert(auditionMessages[0].find("PD object text: oc~ 440") != std::string::npos);
    const auto auditionLoc = pd_diagnostics::inferEditorLine(
        "oc~ 440;\n*~ 0.08;\ndac~ 1 2;\n", auditionMessages[0]);
    assert(auditionLoc.line == 1 && auditionLoc.candidates.empty());

    // A generic error with no object evidence must not claim all three rows.
    const auto unknown = pd_diagnostics::inferEditorLine(
        "oc~ 440;\n*~ 0.08;\ndac~ 1 2;\n", "error: ... couldn't create");
    assert(unknown.line == 0 && unknown.candidates.empty());
    std::cout << "PASS: preload and audition Pd hook -> shared console -> editor line 1\n";
}
