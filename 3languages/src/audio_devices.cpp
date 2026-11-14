#include "audio_devices.h"
#include "process.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <regex>
#include <thread>
#include <utility>

std::vector<AudioDevice> listCsoundDevices(const std::string& executable,
                                           const std::string& backend,
                                           bool output,
                                           std::string& error) {
    error.clear();
    ProcessRunner process;
    const std::string exe = executable.empty() ? "csound" : executable;
    const std::string command = "\"" + exe + "\" -+rtaudio=" + backend + (output ? " --devices=out" : " --devices=in");
    if (!process.start(command)) {
        error = process.lastError();
        return {};
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!process.finished() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool timedOut = !process.finished();
    std::vector<std::string> lines = process.takeOutput();
    process.stop();
    if (timedOut) {
        error = "Csound timed out while listing audio devices.";
        return {};
    }

    static const std::regex deviceLine(R"(^\s*(\d+)\s*:\s*(.*?)\s*$)");
    std::vector<AudioDevice> devices;
    for (const std::string& line : lines) {
        std::smatch match;
        if (!std::regex_match(line, match, deviceLine)) continue;
        try {
            AudioDevice d;
            d.id = std::stoi(match[1].str());
            d.name = match[2].str();
            if (!d.name.empty()) devices.push_back(std::move(d));
        } catch (...) {
        }
    }
    if (devices.empty()) {
        error = "Csound returned no device list for this backend. Check that this Csound build includes " + backend + ".";
    }
    return devices;
}
