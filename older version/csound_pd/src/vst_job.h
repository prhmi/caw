#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>
// One VST3 track as the Csound engine thread sees it.
struct VstJob {
    int id = 0;                    // Track::id: the Csound channels are cawvst_<id>_...
    int meterFirst = -1;           // stereo indices in the monitor snapshot, -1 disables metering
    std::string name;              // track name = the audio bus name used by cawsa "name", aL, aR
    std::string path;              // .vst3 file
    std::map<uint32_t, double> parameterState; // stable plugin parameter IDs
    bool instrument = true;        // true = VSTi (MIDI in), false = VSTe (audio in from the bus)
    std::vector<int> pick;         // plugin parameter index of kP1..kP8
    std::string out;               // audio bus name the sound goes to ("" = master only)
    bool outMaster = false;        // with a bus: also to the master
    std::vector<double> init;      // start value of kP1..kP8 (normalized)
};
