#include "audio_devices.h"
#include "caw_audio_host.h"
#include "process_runner.h"
#include <portaudio.h>

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

void listPortAudioDevicePairs(std::vector<AudioDevice>& outputs, std::vector<AudioDevice>& inputs,
                              std::string& outputError, std::string& inputError) {
    outputs.clear(); inputs.clear(); outputError.clear(); inputError.clear();
    std::string initError;
    caw_audio_host::Ref host(&initError);   // never terminates PortAudio under a running engine stream
    if (!host.ok) {
        outputError = inputError = initError; return;
    }
    const int count = Pa_GetDeviceCount();
    if (count < 0) {
        outputError = inputError = Pa_GetErrorText(count); return;
    }
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* d = Pa_GetDeviceInfo(i);
        if (!d || !d->name) continue;
        if (d->maxOutputChannels <= 0 && d->maxInputChannels <= 0) continue;
        const PaHostApiInfo* host = Pa_GetHostApiInfo(d->hostApi);
        if (!host || !host->name) continue;
        AudioDevice dev;
        dev.id = i;
        dev.name = std::string(d->name) + " [" + host->name + ", " + std::to_string(d->maxInputChannels) + " in, " +
                   std::to_string(d->maxOutputChannels) + " out]";
        if (d->maxOutputChannels > 0) outputs.push_back(dev);
        if (d->maxInputChannels > 0) inputs.push_back(std::move(dev));
    }
    if (outputs.empty()) outputError = "PortAudio found no output devices.";
    if (inputs.empty()) inputError = "PortAudio found no input devices.";
}
std::vector<AudioDevice> listPortAudioDevices(bool output, std::string& error) {
    std::vector<AudioDevice> outputs, inputs; std::string outError, inError;
    listPortAudioDevicePairs(outputs, inputs, outError, inError);
    error = output ? outError : inError;
    return output ? std::move(outputs) : std::move(inputs);
}
