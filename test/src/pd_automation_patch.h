#pragma once
#include <filesystem>
#include <fstream>
#include <string>

namespace pd_automation_patch {
// Only vanilla objects. [r $0-cawget-input1] receives the automation lane value (the + menu
// lists "input1" because of it), [s $0-cawset-input1] lets the patch write the lane while W
// is armed. CAW enables DSP when opening the patch.
inline constexpr const char* source =
    "#N canvas 200 100 620 420 10;\n"
    "#X obj 60 60 r \\$0-cawget-input1;\n"
    "#X obj 60 110 * 660;\n"
    "#X obj 60 150 + 110;\n"
    "#X obj 60 190 osc~ 440;\n"
    "#X obj 60 230 *~ 0.08;\n"
    "#X obj 60 280 dac~ 1 2;\n"
    "#X text 40 330 CAW automation example: the input1 lane value (0..1) sets the pitch (110..770 Hz).;\n"
    "#X text 40 356 The channel input1 appears in the track + menu as an automation lane.;\n"
    "#X floatatom 380 110 5 0 1 0 - - -;\n"
    "#X obj 380 160 s \\$0-cawset-input1;\n"
    "#X text 350 210 Send a number here (0..1) to write the lane while W is armed.;\n"
    "#X connect 0 0 1 0;\n"
    "#X connect 1 0 2 0;\n"
    "#X connect 2 0 3 0;\n"
    "#X connect 3 0 4 0;\n"
    "#X connect 4 0 5 0;\n"
    "#X connect 4 0 5 1;\n"
    "#X connect 8 0 9 0;\n";
// Generated example lives in the CAW temporary folder, not the project.
// Never intentionally overwrite an existing file; copies can be edited in PD.
inline bool create(const std::filesystem::path& folder, std::string& result, std::string& error) {
    result.clear(); error.clear(); std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec) || ec) { error="CAW's temporary folder is unavailable."; return false; }
    for (int i=0; i<10000; ++i) {
        auto name = std::string("CAW-automation-example") + (i ? "-"+std::to_string(i) : "") + ".pd";
        auto path=folder / name;
        const bool exists=std::filesystem::exists(path,ec);
        if (ec) { error="Could not inspect the example file location."; return false; }
        if (exists) continue;
        std::ofstream out(path,std::ios::binary);
        if (!out) { error="Could not create the example patch."; return false; }
        out << source; out.close();
        if (!out) { std::filesystem::remove(path,ec); error="Could not write the complete example patch."; return false; }
        result=path.u8string(); return true;
    }
    error="Too many example patches in CAW's temporary folder.";return false;
}
}
