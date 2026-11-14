#pragma once
#include <string>
#include <vector>

struct AudioDevice {
    int id = -1;
    std::string name;
};

// Csound numbers devices per real-time audio module. On Windows, ASIO,
// WASAPI and DirectSound devices are enumerated by PortAudio, not as modules.
std::vector<AudioDevice> listCsoundDevices(const std::string& executable,
                                           const std::string& backend,
                                           bool output,
                                           std::string& error);
