#pragma once
// Diagnostic switches read from environment variables (set them before starting caw.exe, e.g.  set CAW_NO_MON=1).
#include <cstdlib>
#include <cstring>
inline bool cawEnv(const char* name) { const char* v = std::getenv(name); return v && *v && std::strcmp(v, "0") != 0; }
inline int cawEnvInt(const char* name, int def) { const char* v = std::getenv(name); return (v && *v) ? std::atoi(v) : def; }
