#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "osc_sender.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#pragma comment(lib, "ws2_32.lib")

namespace {
void padString(std::string& buf, const std::string& s) {
    buf += s;
    buf.push_back('\0');
    while (buf.size() % 4) buf.push_back('\0');
}
void putFloat(std::string& buf, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    u = htonl(u);
    buf.append(reinterpret_cast<const char*>(&u), 4);
}
}  // namespace

bool sendOsc(const std::vector<OscMsg>& messages) {
    static SOCKET sock = INVALID_SOCKET;
    static bool started = false;
    if (!started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        started = true;
    }
    if (sock == INVALID_SOCKET) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) return false;
    }
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons((u_short)kCawOscPort);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = true;
    for (const OscMsg& m : messages) {
        std::string buf;
        padString(buf, m.address);
        std::string tags = ",s";
        tags.append(m.values.size(), 'f');
        padString(buf, tags);
        padString(buf, m.name);
        for (float v : m.values) putFloat(buf, v);
        if (sendto(sock, buf.data(), (int)buf.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == SOCKET_ERROR) ok = false;
    }
    return ok;
}

// ---------------------------------------------------------------- monitor receiver
namespace {
SOCKET g_monSock = INVALID_SOCKET;

uint32_t getU32(const char* p) {
    uint32_t u;
    std::memcpy(&u, p, 4);
    return ntohl(u);
}
size_t paddedLen(size_t n) { return (n + 4) & ~(size_t)3; }   // string + terminating zero, padded to 4

void parseMonitorPacket(const char* d, size_t n, std::vector<MonPacket>& out) {
    if (n < 8) return;
    const size_t addrLen = strnlen(d, n);
    if (addrLen >= n) return;
    const std::string addr(d, addrLen);
    size_t pos = paddedLen(addrLen);
    if (pos >= n || d[pos] != ',') return;
    const size_t tagLen = strnlen(d + pos, n - pos);
    const std::string tags(d + pos + 1, tagLen - 1);
    pos += paddedLen(tagLen);
    MonPacket m;
    if (addr == "/caw/m") m.tag = 'M';
    else if (addr == "/caw/s") m.tag = 'S';
    else if (addr == "/caw/l") m.tag = 'L';
    else if (addr == "/caw/r") m.tag = 'R';
    else return;
    bool first = true;
    for (char t : tags) {
        if (pos + 4 > n) return;
        if (t == 'i') {
            const int v = (int)getU32(d + pos);
            if (first) m.start = v; else m.values.push_back((float)v);
        } else if (t == 'f') {
            const uint32_t u = getU32(d + pos);
            float f;
            std::memcpy(&f, &u, 4);
            if (first) m.start = (int)f; else m.values.push_back(f);
        } else {
            return;
        }
        first = false;
        pos += 4;
    }
    out.push_back(std::move(m));
}
}  // namespace

bool monitorListenStart() {
    if (g_monSock != INVALID_SOCKET) return true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    int rcv = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcv), sizeof(rcv));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)kCawMonPort);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR) {
        closesocket(s);
        return false;
    }
    g_monSock = s;
    return true;
}

void monitorPoll(std::vector<MonPacket>& out) {
    if (g_monSock == INVALID_SOCKET) return;
    char buf[8192];
    for (int guard = 0; guard < 2000; ++guard) {
        const int n = recvfrom(g_monSock, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n <= 0) break;
        parseMonitorPacket(buf, (size_t)n, out);
    }
}
