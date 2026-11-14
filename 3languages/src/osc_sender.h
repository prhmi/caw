#pragma once
#include <string>
#include <vector>

// Tiny OSC-over-UDP sender (localhost). The running Csound engine listens with OSClisten, so the timeline can
// trigger instruments without any stdin trick.
//   /caw/i    "sfffff"  instrument name, delay, dur, p4, p5, (unused)  -> event "i"
//   /caw/a    "sffff"   channel name, delay, dur, startValue, endValue -> automation ramp
//   /caw/off  "s"       instrument name                               -> turnoff2
constexpr int kCawOscPort = 7770;

struct OscMsg {
    std::string address;
    std::string name;
    std::vector<float> values;
};

bool sendOsc(const std::vector<OscMsg>& messages);   // false if the socket failed

// ---- monitor feed: the running Csound engine (CawMon instrument) sends meter / scope / spectrum frames to the app over UDP
constexpr int kCawMonPort = 7771;
struct MonPacket {
    char tag = 'M';              // M = peaks, S = spectrum bands, L / R = scope points
    int start = 0;               // index of the first value
    std::vector<float> values;
};
bool monitorListenStart();                       // opens the UDP port once (false = could not bind)
void monitorPoll(std::vector<MonPacket>& out);   // everything received since the last call
