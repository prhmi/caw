#pragma once
#include <memory>
#include <string>
#include <vector>

// Runs a console program (csound.exe) hidden, captures its stdout + stderr line by line.
class ProcessRunner {
public:
    ProcessRunner();
    ~ProcessRunner();
    ProcessRunner(const ProcessRunner&) = delete;
    ProcessRunner& operator=(const ProcessRunner&) = delete;

    bool start(const std::string& commandLine, bool withInput = false);   // false -> see lastError(); withInput opens a stdin pipe
    bool writeInput(const std::string& text);     // text for the program's stdin (needs withInput)
    void setHighPriority(bool on);                // the next start() runs the program at high CPU priority (the real-time audio engine: fewer dropouts)
    void stop();                                  // kills the process if needed and cleans up
    bool active() const;                          // started and not yet stopped
    bool running() const;                         // process is still alive
    bool finished() const;                        // process exited and all its output was read
    int exitCode() const;
    std::vector<std::string> takeOutput();        // new lines since the last call
    const std::string& lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
