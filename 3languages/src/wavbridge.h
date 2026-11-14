#pragma once
#include <string>

// Csound-free player for Pure Data / SuperCollider clips.
// Pd and SC clips are rendered to wav files (caw_pd/<clip>.wav). Normally a running Csound plays those files.
// When only Pd and / or SC is started, nobody needs Csound: this small mixer plays the same files straight to the sound card.
namespace wavbridge {
bool start(int sampleRate, std::string& error);    // opens the default output device and starts the mixer thread
void stop();                                       // stops everything and closes the device
bool running();
double latencyMs();                                // time between the timeline and the sound card output of this player
// play a wav: after `delay` seconds, skip `offset` seconds of the file, play at most `dur` seconds. `tag` = clip name (for off()).
bool play(const std::string& path, const std::string& tag, double delay, double dur, double offset, std::string& error);
void off(const std::string& tag);                  // short fade out of every voice of this clip ("" = all voices)
}
