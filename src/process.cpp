#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "process.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

struct ProcessRunner::Impl {
    HANDLE proc = nullptr;
    HANDLE pipeRead = nullptr;
    HANDLE pipeWrite = nullptr;   // our end of the child's stdin
    std::thread reader;
    std::mutex m;
    std::vector<std::string> lines;
    std::atomic<bool> readerDone{true};
    int exitCode = 0;
    std::string err;
    bool highPriority = false;
};

ProcessRunner::ProcessRunner() : d(new Impl) {}
void ProcessRunner::setHighPriority(bool on) { d->highPriority = on; }
ProcessRunner::~ProcessRunner() { stop(); }

bool ProcessRunner::start(const std::string& commandLine, bool withInput) {
    stop();
    d->err.clear();

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        d->err = "CreatePipe failed";
        return false;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE inRd = nullptr, inWr = nullptr;
    if (withInput) {
        if (!CreatePipe(&inRd, &inWr, &sa, 65536)) {
            CloseHandle(rd);
            CloseHandle(wr);
            d->err = "CreatePipe (stdin) failed";
            return false;
        }
        SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdInput = withInput ? inRd : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::string cmd = commandLine;  // CreateProcess needs a writable buffer
    BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (ok && d->highPriority) SetPriorityClass(pi.hProcess, HIGH_PRIORITY_CLASS);
    CloseHandle(wr);
    if (inRd) CloseHandle(inRd);
    if (!ok) {
        DWORD e = GetLastError();
        CloseHandle(rd);
        if (inWr) CloseHandle(inWr);
        std::string prog = commandLine;
        if (!prog.empty() && prog[0] == '"') { const size_t q = prog.find('"', 1); prog = prog.substr(1, q == std::string::npos ? std::string::npos : q - 1); }
        else prog = prog.substr(0, prog.find(' '));
        d->err = "could not start \"" + prog + "\" (Windows error " + std::to_string(e) +
                 (e == 2 ? ": the program was not found" : "") +
                 "). Install it, or set its full path in Settings (Csound / Pure Data / SuperCollider program).";
        return false;
    }
    CloseHandle(pi.hThread);
    d->proc = pi.hProcess;
    d->pipeRead = rd;
    d->pipeWrite = inWr;
    d->readerDone = false;

    Impl* impl = d.get();
    d->reader = std::thread([impl]() {
        char buf[4096];
        DWORD n = 0;
        std::string cur;
        auto flush = [&]() {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            std::lock_guard<std::mutex> lock(impl->m);
            impl->lines.push_back(cur);
            cur.clear();
        };
        while (ReadFile(impl->pipeRead, buf, sizeof(buf), &n, nullptr) && n > 0) {
            for (DWORD i = 0; i < n; ++i) {
                if (buf[i] == '\n') flush();
                else cur.push_back(buf[i]);
            }
        }
        if (!cur.empty()) flush();
        impl->readerDone = true;
    });
    return true;
}

void ProcessRunner::stop() {
    if (d->pipeWrite) { CloseHandle(d->pipeWrite); d->pipeWrite = nullptr; }
    if (d->proc) {
        if (WaitForSingleObject(d->proc, 0) == WAIT_TIMEOUT) {
            TerminateProcess(d->proc, 1);
            WaitForSingleObject(d->proc, 2000);
        }
        DWORD code = 0;
        if (GetExitCodeProcess(d->proc, &code)) d->exitCode = (int)code;
    }
    if (d->reader.joinable()) d->reader.join();
    if (d->pipeRead) { CloseHandle(d->pipeRead); d->pipeRead = nullptr; }
    if (d->proc) { CloseHandle(d->proc); d->proc = nullptr; }
}

bool ProcessRunner::writeInput(const std::string& text) {
    if (!d->pipeWrite || text.empty()) return false;
    DWORD written = 0;
    return WriteFile(d->pipeWrite, text.data(), (DWORD)text.size(), &written, nullptr) && written == text.size();
}

bool ProcessRunner::active() const { return d->proc != nullptr; }

bool ProcessRunner::running() const {
    return d->proc && WaitForSingleObject(d->proc, 0) == WAIT_TIMEOUT;
}

bool ProcessRunner::finished() const {
    return d->proc && !running() && d->readerDone.load();
}

int ProcessRunner::exitCode() const {
    if (d->proc) {
        DWORD c = 0;
        if (GetExitCodeProcess(d->proc, &c) && c != STILL_ACTIVE) return (int)c;
    }
    return d->exitCode;
}

std::vector<std::string> ProcessRunner::takeOutput() {
    std::lock_guard<std::mutex> lock(d->m);
    std::vector<std::string> out;
    out.swap(d->lines);
    return out;
}

const std::string& ProcessRunner::lastError() const { return d->err; }
