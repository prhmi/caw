// Last-resort crash record: a small text line plus a minidump next to caw.exe.
// The app is a DAW hosting third-party plugins; when one of them faults, Windows Error
// Reporting often records nothing usable. caw_crash.txt says which module faulted, and
// caw_crash.dmp opens in Visual Studio / WinDbg with the exact call stack.
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <exception>
#include <string>
#include <atomic>
#include <functional>

namespace cawcrash {

inline std::string exeDir() {
    char exe[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, exe, MAX_PATH)) return std::string();
    std::string base = exe;
    const size_t slash = base.find_last_of("\\/");
    if (slash != std::string::npos) base.resize(slash + 1);
    return base;
}

inline void writeRecord(EXCEPTION_POINTERS* ep, const char* why) {
    const std::string base = exeDir();
    if (base.empty()) return;
    if (FILE* f = fopen((base + "caw_crash.txt").c_str(), "a")) {
        const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
        void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
        char module[MAX_PATH] = "?";
        if (addr) {
            HMODULE m = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(addr), &m) && m)
                GetModuleFileNameA(m, module, MAX_PATH);
        }
        std::fprintf(f, "CAW crash (%s): code=0x%08lx addr=%p module=%s pid=%lu\n", why,
                     static_cast<unsigned long>(code), addr, module,
                     static_cast<unsigned long>(GetCurrentProcessId()));
        std::fclose(f);
    }
    HANDLE h = CreateFileA((base + "caw_crash.dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = ep;
    info.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                      static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                                 MiniDumpWithUnloadedModules),
                      ep ? &info : nullptr, nullptr, nullptr);
    CloseHandle(h);
}

inline LONG WINAPI filter(EXCEPTION_POINTERS* ep) {
    writeRecord(ep, "unhandled exception");
    return EXCEPTION_EXECUTE_HANDLER;
}

// SEH filter for GUARDED third-party plugin calls (vst3_host.cpp): records the fault
// (rate-limited: a plugin that faults on every call would otherwise flood caw_crash.txt and
// write a minidump each time) and lets the caller continue instead of killing CAW.
inline std::function<void(const std::string&)> sehConsole;   // set by the app: recovered plugin faults surface in the console
inline LONG WINAPI sehSwallowRecord(EXCEPTION_POINTERS* ep) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
        writeRecord(ep, "plugin call (recovered)");
        if (sehConsole) {
            void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
            char module[MAX_PATH] = "?";
            if (addr) {
                HMODULE m = nullptr;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCSTR>(addr), &m) && m)
                    GetModuleFileNameA(m, module, MAX_PATH);
            }
            sehConsole(std::string("[caw] A plugin faulted inside one of its calls (access violation) - CAW recovered and continued. Module: ") + module);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

inline void install() {
    SetUnhandledExceptionFilter(filter);
    std::set_terminate([] {
        writeRecord(nullptr, "std::terminate");
        TerminateProcess(GetCurrentProcess(), 3);
    });
}

}   // namespace cawcrash
#endif
