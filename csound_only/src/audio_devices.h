#pragma once
#include <string>
#include <vector>

struct AudioDevice {
    int id = -1;
    std::string name;
};

// Csound numbers devices per real-time audio module. ASIO, WASAPI, DirectSound,
// WDM-KS (Windows), Core Audio (macOS), ALSA / JACK / OSS (Linux) are PortAudio
// host APIs: they are enumerated by PortAudio, not as separate Csound modules.
std::vector<AudioDevice> listCsoundDevices(const std::string& executable,
                                           const std::string& backend,
                                           bool output,
                                           std::string& error);

// Every device of every host API that CAW's OWN PortAudio build can open (ASIO, WASAPI, DirectSound, MME, WDM-KS, Core Audio, ALSA, JACK ...).
// id = PortAudio device index, name = "Device name [Host API, N in, M out]" (the label format CAW's audio output matches on).
// This is the list CAW plays through, so no driver type is empty just because Csound's PortAudio lacks it.
std::vector<AudioDevice> listPortAudioDevices(bool output, std::string& error);
