#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "process_runner.h"
#include "caw_toggles.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _WIN32
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
    if (!CreatePipe(&rd, &wr, &sa, cawEnv("CAW_OLD_PIPE") ? 0 : (1 << 20))) {
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
    if (ok && d->highPriority) {
        SetPriorityClass(pi.hProcess, HIGH_PRIORITY_CLASS);
        // Windows 10/11 can treat a window-less child process as "background": it gets power-throttled, put on efficiency cores and gets
        // a coarse timer. For a live audio engine that means dropouts at small buffers. Switch all of that off for the Csound process.
        struct PowerThrottle { ULONG Version; ULONG ControlMask; ULONG StateMask; } pt = {1, 0x1 | 0x4, 0};   // EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION, state 0 = never throttle
        typedef BOOL(WINAPI * SetProcInfoFn)(HANDLE, int, LPVOID, DWORD);
        if (HMODULE k32 = GetModuleHandleA("kernel32.dll"))
            if (SetProcInfoFn fn = (SetProcInfoFn)GetProcAddress(k32, "SetProcessInformation"))
                fn(pi.hProcess, 4 /* ProcessPowerThrottling */, &pt, sizeof(pt));
        SetProcessPriorityBoost(pi.hProcess, FALSE);   // keep the foreground priority boost
    }
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
                 "). Install it, or set its full path in Settings (Csound program).";
        return false;
    }
    CloseHandle(pi.hThread);
    d->proc = pi.hProcess;
    d->pipeRead = rd;
    d->pipeWrite = inWr;
    d->readerDone = false;

    Impl* impl = d.get();
    d->reader = std::thread([impl]() {
        if (!cawEnv("CAW_OLD_PIPE")) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);   // never let a full stdout pipe block Csound
        char buf[4096];
        DWORD n = 0;
        std::string cur;
        std::vector<std::string> batch;
        auto flush = [&]() {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            batch.push_back(std::move(cur));
            cur.clear();
        };
        auto deliver = [&]() {
            if (batch.empty()) return;
            std::lock_guard<std::mutex> lock(impl->m);
            for (std::string& line : batch) impl->lines.push_back(std::move(line));
            batch.clear();
        };
        while (ReadFile(impl->pipeRead, buf, sizeof(buf), &n, nullptr) && n > 0) {
            for (DWORD i = 0; i < n; ++i) {
                if (buf[i] == '\n') flush();
                else cur.push_back(buf[i]);
            }
            deliver();
        }
        if (!cur.empty()) flush();
        deliver();
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
#else

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

struct ProcessRunner::Impl {
    pid_t pid = -1;
    int outFd = -1;     // we read the program's stdout + stderr here
    int inFd = -1;      // we write the program's stdin here
    std::thread reader;
    std::mutex m;
    std::vector<std::string> lines;
    std::atomic<bool> readerDone{true};
    std::atomic<bool> exited{false};
    int exitCode = 0;
    std::string err;
    bool highPriority = false;
};

ProcessRunner::ProcessRunner() : d(new Impl) {}
void ProcessRunner::setHighPriority(bool on) { d->highPriority = on; }
ProcessRunner::~ProcessRunner() { stop(); }

static void reapIfDone(ProcessRunner::Impl* d) {
    if (d->pid <= 0 || d->exited) return;
    int status = 0;
    const pid_t r = waitpid(d->pid, &status, WNOHANG);
    if (r == d->pid) {
        d->exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1);
        d->exited = true;
    }
}

bool ProcessRunner::start(const std::string& commandLine, bool withInput) {
    stop();
    d->err.clear();
    d->exited = false;
    d->exitCode = 0;

    int outPipe[2] = {-1, -1}, inPipe[2] = {-1, -1};
    if (pipe(outPipe) != 0) { d->err = "pipe failed"; return false; }
    if (withInput && pipe(inPipe) != 0) { close(outPipe[0]); close(outPipe[1]); d->err = "pipe (stdin) failed"; return false; }

    // Parse the first shell word, including single-quoted paths and escaped quotes.
    std::string prog;
    char quote = 0;
    for (size_t i = 0; i < commandLine.size(); ++i) {
        const char c = commandLine[i];
        if (!quote && (c == ' ' || c == '\t')) break;
        if (!quote && (c == '\'' || c == '"')) { quote = c; continue; }
        if (quote && c == quote) { quote = 0; continue; }
        if (c == '\\' && quote != '\'' && i + 1 < commandLine.size()) {
            prog += commandLine[++i];
            continue;
        }
        prog += c;
    }

    // A quick check so a missing program gives the same helpful message as on Windows
    {
        bool found = false;
        if (prog.find('/') != std::string::npos) found = access(prog.c_str(), X_OK) == 0;
        else {
            const char* path = std::getenv("PATH");
            std::string p = path ? path : "";
            p += ":/usr/local/bin:/opt/homebrew/bin:/opt/local/bin:/usr/bin:/snap/bin";
            size_t a = 0;
            while (a <= p.size() && !found) {
                size_t b = p.find(':', a);
                if (b == std::string::npos) b = p.size();
                if (b > a && access((p.substr(a, b - a) + "/" + prog).c_str(), X_OK) == 0) found = true;
                a = b + 1;
            }
        }
        if (!found) {
            close(outPipe[0]); close(outPipe[1]);
            if (inPipe[0] >= 0) { close(inPipe[0]); close(inPipe[1]); }
            d->err = "could not start \"" + prog + "\" (the program was not found). Install it, or set its full path in Settings (Csound program).";
            return false;
        }
    }

    const bool high = d->highPriority;
    const pid_t pid = fork();
    if (pid < 0) {
        close(outPipe[0]); close(outPipe[1]);
        if (inPipe[0] >= 0) { close(inPipe[0]); close(inPipe[1]); }
        d->err = std::string("fork failed: ") + std::strerror(errno);
        return false;
    }
    if (pid == 0) {   // child
        setpgid(0, 0);   // own process group: stop() can kill the whole tree
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(outPipe[1], STDERR_FILENO);
        if (withInput) dup2(inPipe[0], STDIN_FILENO);
        else { const int nul = open("/dev/null", O_RDONLY); if (nul >= 0) { dup2(nul, STDIN_FILENO); close(nul); } }
        close(outPipe[0]); close(outPipe[1]);
        if (withInput) { close(inPipe[0]); close(inPipe[1]); }
        if (high) { errno = 0; (void)setpriority(PRIO_PROCESS, 0, -5); }   // needs privileges on most systems: silently ignored otherwise
        // PATH of a GUI app can be short: extend it so "csound" is found like in a terminal
        const char* oldPath = std::getenv("PATH");
        std::string np = std::string(oldPath ? oldPath : "") + ":/usr/local/bin:/opt/homebrew/bin:/opt/local/bin:/usr/bin:/snap/bin";
        setenv("PATH", np.c_str(), 1);
        execl("/bin/sh", "sh", "-c", commandLine.c_str(), (char*)nullptr);
        _exit(127);
    }
    setpgid(pid, pid);
    close(outPipe[1]);
    if (withInput) close(inPipe[0]);
    d->pid = pid;
    d->outFd = outPipe[0];
    d->inFd = withInput ? inPipe[1] : -1;
    if (d->inFd >= 0) { const int fl = fcntl(d->inFd, F_GETFD); fcntl(d->inFd, F_SETFD, fl | FD_CLOEXEC); }
    signal(SIGPIPE, SIG_IGN);   // writing to a program that has ended must not kill CAW
    d->readerDone = false;

    Impl* impl = d.get();
    d->reader = std::thread([impl]() {
        char buf[4096];
        std::string cur;
        std::vector<std::string> batch;
        auto flush = [&]() {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            batch.push_back(std::move(cur));
            cur.clear();
        };
        auto deliver = [&]() {
            if (batch.empty()) return;
            std::lock_guard<std::mutex> lock(impl->m);
            for (std::string& line : batch) impl->lines.push_back(std::move(line));
            batch.clear();
        };
        for (;;) {
            const ssize_t n = read(impl->outFd, buf, sizeof(buf));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '\n') flush();
                else cur.push_back(buf[i]);
            }
            deliver();
        }
        if (!cur.empty()) flush();
        deliver();
        impl->readerDone = true;
    });
    return true;
}

void ProcessRunner::stop() {
    if (d->inFd >= 0) { close(d->inFd); d->inFd = -1; }
    if (d->pid > 0) {
        reapIfDone(d.get());
        if (!d->exited) {
            kill(-d->pid, SIGTERM);
            kill(d->pid, SIGTERM);
            for (int i = 0; i < 100 && !d->exited; ++i) {   // up to ~1 s to end gently
                usleep(10000);
                reapIfDone(d.get());
            }
            if (!d->exited) {
                kill(-d->pid, SIGKILL);
                kill(d->pid, SIGKILL);
                int status = 0;
                waitpid(d->pid, &status, 0);
                d->exitCode = 137;
                d->exited = true;
            }
        }
    }
    if (d->reader.joinable()) d->reader.join();
    if (d->outFd >= 0) { close(d->outFd); d->outFd = -1; }
    d->pid = -1;
}

bool ProcessRunner::writeInput(const std::string& text) {
    if (d->inFd < 0 || text.empty()) return false;
    size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = write(d->inFd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

bool ProcessRunner::active() const { return d->pid > 0; }

bool ProcessRunner::running() const {
    if (d->pid <= 0) return false;
    reapIfDone(d.get());
    return !d->exited;
}

bool ProcessRunner::finished() const {
    return d->pid > 0 && !running() && d->readerDone.load();
}

int ProcessRunner::exitCode() const {
    if (d->pid > 0) reapIfDone(d.get());
    return d->exitCode;
}

std::vector<std::string> ProcessRunner::takeOutput() {
    std::lock_guard<std::mutex> lock(d->m);
    std::vector<std::string> out;
    out.swap(d->lines);
    return out;
}

const std::string& ProcessRunner::lastError() const { return d->err; }
#endif
