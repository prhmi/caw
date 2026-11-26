#pragma once
// CAW CABLES in Pure Data. A patch uses two objects:
//   [cawr~ <name>]   receive the stereo cable <name>   (two signal outlets: L R)
//   [caws~ <name>]   send a stereo pair into the cable <name> (two signal inlets: L R; never heard on the master)
// They are abstractions CAW writes next to the patch (cawr~.pd / caws~.pd). For every clip CAW also
// writes a bridge patch that maps each name onto fixed libpd channels (adc~ 9..24 / dac~ 9..24) the
// host moves in and out of the cables. [adc~ 1..8] stays the soundcard input (the microphone) and
// [dac~ 1..8] the track's sound.
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>
#include "pd_control_syntax.h"

namespace pd_cables {
inline const char* kRecvAbstraction =
    "#N canvas 0 0 520 200 12;\n"
    "#X text 10 6 cawr~ <name> - receive the CAW cable <name> (stereo: left / right outlets). Made by CAW.;\n"
    "#X obj 10 60 r~ cawcable-$1-L;\n"
    "#X obj 250 60 r~ cawcable-$1-R;\n"
    "#X obj 10 120 outlet~;\n"
    "#X obj 250 120 outlet~;\n"
    "#X connect 1 0 3 0;\n"
    "#X connect 2 0 4 0;\n";
inline const char* kSendAbstraction =
    "#N canvas 0 0 520 200 12;\n"
    "#X text 10 6 caws~ <name> - send a stereo pair into the CAW cable <name> (left / right inlets). Made by CAW.;\n"
    "#X obj 10 60 inlet~;\n"
    "#X obj 250 60 inlet~;\n"
    "#X obj 10 120 s~ cawcable-$1-out-L;\n"
    "#X obj 250 120 s~ cawcable-$1-out-R;\n"
    "#X connect 1 0 3 0;\n"
    "#X connect 2 0 4 0;\n";
inline bool writeIfChanged(const std::filesystem::path& p, const std::string& text) {
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) {
        std::ifstream in(p, std::ios::binary);
        std::string cur((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (cur == text) return true;
    }
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return (bool)out;
}
// Writes cawr~.pd / caws~.pd beside the patch (only when it uses them) and the clip's bridge patch.
// Returns the bridge path ("" = the patch uses no cables). The channel numbers: receive pair k
// (1..8) = adc~ 9+2(k-1) / 10+2(k-1), send pair k = dac~ 9+2(k-1) / 10+2(k-1).
inline std::string prepare(const std::string& patchPath, int clipId, const std::vector<pd_control_syntax::BusLink>& links) {
    if (links.empty()) return {};
    const std::filesystem::path dir = std::filesystem::u8path(patchPath).parent_path();
    writeIfChanged(dir / "cawr~.pd", kRecvAbstraction);
    writeIfChanged(dir / "caws~.pd", kSendAbstraction);
    std::string b = "#N canvas 0 0 600 400 12;\n#X text 10 6 CAW cable bridge for clip " + std::to_string(clipId) + " - made by CAW;\n";
    int obj = 1, y = 40;
    std::string conns;
    for (const auto& l : links) {
        const int base = 9 + 2 * (l.channel - 1);
        if (!l.send) {
            b += "#X obj 10 " + std::to_string(y) + " adc~ " + std::to_string(base) + " " + std::to_string(base + 1) + ";\n";
            b += "#X obj 10 " + std::to_string(y + 30) + " s~ cawcable-" + l.bus + "-L;\n";
            b += "#X obj 200 " + std::to_string(y + 30) + " s~ cawcable-" + l.bus + "-R;\n";
            conns += "#X connect " + std::to_string(obj) + " 0 " + std::to_string(obj + 1) + " 0;\n";
            conns += "#X connect " + std::to_string(obj) + " 1 " + std::to_string(obj + 2) + " 0;\n";
        } else {
            b += "#X obj 10 " + std::to_string(y) + " r~ cawcable-" + l.bus + "-out-L;\n";
            b += "#X obj 200 " + std::to_string(y) + " r~ cawcable-" + l.bus + "-out-R;\n";
            b += "#X obj 10 " + std::to_string(y + 30) + " dac~ " + std::to_string(base) + " " + std::to_string(base + 1) + ";\n";
            conns += "#X connect " + std::to_string(obj) + " 0 " + std::to_string(obj + 2) + " 0;\n";
            conns += "#X connect " + std::to_string(obj + 1) + " 0 " + std::to_string(obj + 2) + " 1;\n";
        }
        obj += 3; y += 70;
    }
    b += conns;
    const std::filesystem::path bridge = dir / ("caw_bridge_" + std::to_string(clipId) + ".pd");
    if (!writeIfChanged(bridge, b)) return {};
    return bridge.u8string();
}
}  // namespace pd_cables
