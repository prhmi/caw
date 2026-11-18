#pragma once
#include <filesystem>
#include <fstream>
#include <string>

namespace pd_test_patch {
// Only vanilla signal objects. CAW enables DSP when opening the patch.
inline constexpr const char* source =
    "#N canvas 200 100 500 320 10;\n"
    "#X obj 90 75 osc~ 440;\n"
    "#X obj 90 120 *~ 0.08;\n"
    "#X obj 90 170 dac~ 1 2;\n"
    "#X text 60 230 CAW test tone: 440 Hz at low level.;\n"
    "#X connect 0 0 1 0;\n"
    "#X connect 1 0 2 0;\n"
    "#X connect 1 0 2 1;\n";
// Generated example lives in the CAW temporary folder, not the project.
// Never intentionally overwrite an existing file; copies can be edited in PD.
inline bool create(const std::filesystem::path& folder, std::string& result, std::string& error) {
    result.clear(); error.clear(); std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec) || ec) { error="CAW's temporary folder is unavailable."; return false; }
    for (int i=0; i<10000; ++i) {
        auto name = std::string("CAW-test-tone") + (i ? "-"+std::to_string(i) : "") + ".pd";
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
