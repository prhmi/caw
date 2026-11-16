#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

// Streaming, interleaved little-endian WAV sink. No audio device required.
// Formats 0,1,2 are PCM16, PCM24, and IEEE float32. Files >4 GiB fail
// rather than silently wrap their RIFF length.
class CawWavWriter {
    std::ofstream out_;
    int channels_ = 0, bytes_ = 0, format_ = 0;
    uint64_t dataBytes_ = 0;
    void u16(uint16_t n) { char b[2] = {(char)n, (char)(n >> 8)}; out_.write(b, 2); }
    void u32(uint32_t n) { char b[4] = {(char)n, (char)(n >> 8), (char)(n >> 16), (char)(n >> 24)}; out_.write(b, 4); }
public:
    bool open(const std::string& path, int rate, int channels, int format, std::string& error) {
        if (channels < 1 || channels > 64 || rate < 1000 || rate > 384000 || format < 0 || format > 2) {
            error = "Unsupported WAV format."; return false;
        }
        channels_ = channels; format_ = format; bytes_ = format == 0 ? 2 : format == 1 ? 3 : 4;
        out_.open(path, std::ios::binary | std::ios::trunc);
        if (!out_) { error = "Could not create the temporary WAV file."; return false; }
        out_.write("RIFF", 4); u32(0); out_.write("WAVEfmt ", 8); u32(16);
        u16(format == 2 ? 3 : 1); u16((uint16_t)channels); u32((uint32_t)rate);
        u32((uint32_t)(rate * channels * bytes_)); u16((uint16_t)(channels * bytes_)); u16((uint16_t)(bytes_ * 8));
        out_.write("data", 4); u32(0);
        if (!out_) { error = "Could not write WAV header."; return false; }
        return true;
    }
    template<class T>
    bool write(const T* samples, size_t frames, double fullScale, float gain, std::string& error) {
        if (!samples || !out_ || !std::isfinite(fullScale) || fullScale <= 0 || !std::isfinite(gain)) {
            error = "Invalid output audio."; return false;
        }
        const uint64_t add = (uint64_t)frames * (uint64_t)channels_ * (uint64_t)bytes_;
        if (add > 0xffffffffULL - 36ULL || dataBytes_ > 0xffffffffULL - 36ULL - add) {
            error = "WAV export exceeds the 4 GiB RIFF limit."; return false;
        }
        std::vector<char> bytes;
        bytes.reserve((size_t)add);
        auto put32 = [&](uint32_t n) {
            bytes.push_back((char)n); bytes.push_back((char)(n >> 8));
            bytes.push_back((char)(n >> 16)); bytes.push_back((char)(n >> 24));
        };
        for (size_t i = 0; i < frames * (size_t)channels_; ++i) {
            double v = (double)samples[i] / fullScale * gain;
            if (!std::isfinite(v)) v = 0;
            if (format_ == 2) {
                const float f = (float)std::max(-double(std::numeric_limits<float>::max()), std::min(double(std::numeric_limits<float>::max()), v));
                uint32_t bits = 0; std::memcpy(&bits, &f, 4); put32(bits);
            } else {
                v = std::max(-1.0, std::min(1.0, v));
                const int32_t scale = format_ == 0 ? 32768 : 8388608;
                const int32_t q = (int32_t)std::max(-(double)scale, std::min((double)scale - 1.0, std::round(v * scale)));
                const uint32_t u = (uint32_t)q;
                bytes.push_back((char)u); bytes.push_back((char)(u >> 8));
                if (bytes_ == 3) bytes.push_back((char)(u >> 16));
            }
        }
        out_.write(bytes.data(), (std::streamsize)bytes.size());
        dataBytes_ += add;
        if (!out_) { error = "Could not write WAV samples."; return false; }
        return true;
    }
    bool finish(std::string& error) {
        if (!out_) { error = "WAV file is unavailable."; return false; }
        const bool pad = (dataBytes_ & 1u) != 0;
        if (pad) out_.put(0);
        if (dataBytes_ + 36 + (pad ? 1 : 0) > 0xffffffffULL) { error = "WAV export is too large."; return false; }
        out_.seekp(4); u32((uint32_t)(36 + dataBytes_ + (pad ? 1 : 0)));
        out_.seekp(40); u32((uint32_t)dataBytes_);
        out_.flush();
        const bool ok = (bool)out_;
        out_.close();
        if (!ok) error = "Could not finish WAV file.";
        return ok;
    }
};
