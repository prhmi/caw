/*
  CAW host-pumped SuperCollider driver.

  SuperCollider remains GPL-licensed. This backend owns no sound device; CAW's
  PortAudio callback pumps one interleaved block and mixes it with other sources.
*/
#if SC_AUDIO_API == SC_AUDIO_API_EXTERNAL
#include "SC_CoreAudio.h"
#include "SC_HiddenWorld.h"
#include "SC_Prototypes.h"
#include "SC_Time.hpp"
#include "SC_World.h"
#include "SC_WorldOptions.h"
#include "SC_Export.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" int32 server_timeseed() { return timeSeed(); }
extern "C" int64 oscTimeNow() { return OSCTime(getTime()); }
void initializeScheduler() {}

class SC_ExternalDriver final : public SC_AudioDriver {
public:
    static constexpr int cawTrackSlots = 32;
    explicit SC_ExternalDriver(World* world) : SC_AudioDriver(world) { mOSCbuftime = 0; }

    void processBlock(const float* input, int inputChannels, float* output, int outputChannels, int frames,
                      float* trackMeters, int meterSlots) {
        if (!output || frames <= 0 || outputChannels <= 0) return;
        std::fill(output, output + static_cast<size_t>(frames) * outputChannels, 0.f);
        if (trackMeters && meterSlots > 0)
            std::fill(trackMeters, trackMeters + static_cast<size_t>(meterSlots) * 2, 0.f);
        World* world = mWorld;
        const int block = static_cast<int>(world->mBufLength);
        if (block <= 0 || frames % block != 0) {
            // CAW requests PortAudio callback sizes aligned to the SC block size.
            mAudioSync.Signal();
            return;
        }

        mFromEngine.Free();
        mToEngine.Perform();
        mOscPacketsToEngine.Perform();

        const int numBlocks = frames / block;
        float* inputBuses = world->mAudioBus + world->mNumOutputs * block;
        const float* outputBuses = world->mAudioBus;
        int32* inputTouched = world->mAudioBusTouched + world->mNumOutputs;
        const int32* outputTouched = world->mAudioBusTouched;
        int64 oscTime = mOSCbuftime;
        const int64 oscInc = mOSCincrement;

        for (int b = 0; b < numBlocks; ++b, ++world->mBufCounter) {
            const int frameOffset = b * block;
            const int32 bufferCounter = world->mBufCounter;
            for (int channel = 0; input && channel < static_cast<int>(world->mNumInputs) && channel < inputChannels; ++channel) {
                float* destination = inputBuses + channel * block;
                const float* source = input + static_cast<size_t>(frameOffset) * inputChannels + channel;
                for (int frame = 0; frame < block; ++frame) destination[frame] = source[static_cast<size_t>(frame) * inputChannels];
                inputTouched[channel] = bufferCounter;
            }

            const int64 nextTime = oscTime + oscInc;
            while (mScheduler.NextTime() <= nextTime) {
                SC_ScheduledEvent event = mScheduler.Remove();
                event.Perform();
            }
            world->mSampleOffset = 0;
            world->mSubsampleOffset = 0.f;
            World_Run(world);

            for (int channel = 0; channel < outputChannels; ++channel) {
                const bool masterTouched = channel < static_cast<int>(world->mNumOutputs) &&
                                           outputTouched[channel] == bufferCounter;
                const float* masterBus = outputBuses + channel * block;
                float* destination = output + static_cast<size_t>(frameOffset) * outputChannels + channel;
                for (int frame = 0; frame < block; ++frame) {
                    float mixed = masterTouched ? masterBus[frame] : 0.f;
                    for (int slot = 0; trackMeters && slot < meterSlots && channel < 2; ++slot) {
                        const int meterBusBase = static_cast<int>(world->mNumOutputs) - cawTrackSlots * 2;
                        const int busIndex = meterBusBase + slot * 2 + channel;
                        if (busIndex >= static_cast<int>(world->mNumOutputs) ||
                            outputTouched[busIndex] != bufferCounter) continue;
                        const float sample = outputBuses[busIndex * block + frame];
                        mixed += sample;
                        const size_t peakIndex = static_cast<size_t>(slot) * 2 + channel;
                        trackMeters[peakIndex] = std::max(trackMeters[peakIndex], std::fabs(sample));
                    }
                    destination[static_cast<size_t>(frame) * outputChannels] = mixed;
                }
            }
            oscTime = mOSCbuftime = nextTime;
        }
        mAudioSync.Signal();
    }

protected:
    bool DriverSetup(int* frames, double* sampleRate) override {
        *sampleRate = mPreferredSampleRate ? static_cast<double>(mPreferredSampleRate) : 48000.0;
        *frames = mPreferredHardwareBufferFrameSize ? static_cast<int>(mPreferredHardwareBufferFrameSize) : static_cast<int>(mWorld->mBufLength);
        return true;
    }
    bool DriverStart() override { return true; }
    bool DriverStop() override { return true; }
};

SC_AudioDriver* SC_NewAudioDriver(World* world) { return new SC_ExternalDriver(world); }

namespace {
int gCawScCreateError = 0;
}

SCSYNTH_DLLEXPORT_C int caw_sc_last_error() { return gCawScCreateError; }

SCSYNTH_DLLEXPORT_C World* caw_sc_world_new(int sampleRate, int blockSize, int outputChannels,
                                            int firstUdpPort, int* boundUdpPort) {
    gCawScCreateError = 0;
    WorldOptions options;
    options.mBufLength = static_cast<uint32>(std::max(1, blockSize));
    // SuperCollider's language-side ServerOptions rejects maxLogins > 32.
    // Keep the embedded server's /notify reply consistent with that limit.
    options.mMaxLogins = 32;
    options.mNumOutputBusChannels = static_cast<uint32>(std::max(1, outputChannels) +
                                                        SC_ExternalDriver::cawTrackSlots * 2);
    options.mNumInputBusChannels = 0;
    options.mNumAudioBusChannels = std::max<uint32>(1024, options.mNumOutputBusChannels + 256);
    options.mPreferredSampleRate = static_cast<uint32>(std::max(8000, sampleRate));
    options.mPreferredHardwareBufferFrameSize = options.mBufLength;
    options.mRendezvous = false;
    options.mLoadGraphDefs = 0;
    World* world = World_New(&options);
    if (!world) {
        gCawScCreateError = 1;
        return nullptr;
    }
    const int portLimit = std::min(65535, std::max(firstUdpPort, 1024) + 64);
    for (int port = std::max(firstUdpPort, 1024); port <= portLimit; ++port) {
        if (!World_OpenUDP(world, "127.0.0.1", port)) continue;
        if (boundUdpPort) *boundUdpPort = port;
        return world;
    }
    {
        gCawScCreateError = 2;
        World_Cleanup(world, false);
        return nullptr;
    }
}

SCSYNTH_DLLEXPORT_C int caw_sc_abi_version() { return 4; }

SCSYNTH_DLLEXPORT_C void caw_sc_world_process(World* world, const float* input, int inputChannels,
                                               float* output, int outputChannels, int frames,
                                               float* trackMeters, int meterSlots) {
    if (!world || !world->hw || !world->hw->mAudioDriver) {
        if (output && frames > 0 && outputChannels > 0)
            std::fill(output, output + static_cast<size_t>(frames) * outputChannels, 0.f);
        if (trackMeters && meterSlots > 0)
            std::fill(trackMeters, trackMeters + static_cast<size_t>(meterSlots) * 2, 0.f);
        return;
    }
    static_cast<SC_ExternalDriver*>(world->hw->mAudioDriver)->processBlock(
        input, inputChannels, output, outputChannels, frames, trackMeters, meterSlots);
}

SCSYNTH_DLLEXPORT_C void caw_sc_world_free(World* world) {
    if (world) World_Cleanup(world, false);
}
#endif
