#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Owns a Windows pseudo console (ConPTY) and the process attached to it.
//
// The child sees a real terminal, so PSReadLine, VT colours, mouse reporting
// and Ctrl+C all behave exactly as they do under Windows Terminal.
//
// Threading: Start/Write/Resize/Shutdown are called from the GUI thread. The
// output callback fires on a private reader thread and must not block, because
// Shutdown() waits on that thread draining the pipe.
class ConPty
{
public:
    using OutputFn = std::function<void(std::vector<char>)>;
    using ExitFn = std::function<void(DWORD exitCode)>;

    ConPty() = default;
    ~ConPty();

    ConPty(const ConPty&) = delete;
    ConPty& operator=(const ConPty&) = delete;

    // commandLine is passed to CreateProcessW and is modified in place, so a
    // copy is taken here. Returns false and fills err on failure.
    bool Start(const std::wstring& commandLine, SHORT cols, SHORT rows,
               OutputFn onOutput, ExitFn onExit, std::wstring* err);

    void Write(const char* data, size_t len);
    void Resize(SHORT cols, SHORT rows);
    void Shutdown();

    bool Running() const { return m_running.load(std::memory_order_acquire); }

private:
    void ReaderLoop();

    HPCON m_hPC = nullptr;
    HANDLE m_inWrite = INVALID_HANDLE_VALUE;
    HANDLE m_outRead = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION m_pi{};
    LPPROC_THREAD_ATTRIBUTE_LIST m_attrs = nullptr;

    std::thread m_reader;
    std::atomic<bool> m_running{false};
    std::mutex m_writeLock;   // guards m_inWrite against Shutdown closing it

    OutputFn m_onOutput;
    ExitFn m_onExit;
};
