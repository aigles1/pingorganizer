#include "ConPty.h"

namespace
{
    std::wstring LastErrorText(const wchar_t* what)
    {
        const DWORD e = GetLastError();
        wchar_t* buf = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, e, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
        std::wstring msg = std::wstring(what) + L" failed (0x";
        wchar_t hex[16];
        swprintf_s(hex, L"%08lX", e);
        msg += hex;
        msg += L"): ";
        msg += buf ? buf : L"unknown error";
        if (buf)
            LocalFree(buf);
        return msg;
    }

    void CloseIfValid(HANDLE& h)
    {
        if (h != INVALID_HANDLE_VALUE && h != nullptr)
        {
            CloseHandle(h);
            h = INVALID_HANDLE_VALUE;
        }
    }
}

ConPty::~ConPty()
{
    Shutdown();
}

bool ConPty::Start(const std::wstring& commandLine, SHORT cols, SHORT rows,
                   OutputFn onOutput, ExitFn onExit, std::wstring* err)
{
    m_onOutput = std::move(onOutput);
    m_onExit = std::move(onExit);

    HANDLE inRead = INVALID_HANDLE_VALUE;
    HANDLE outWrite = INVALID_HANDLE_VALUE;

    if (!CreatePipe(&inRead, &m_inWrite, nullptr, 0) ||
        !CreatePipe(&m_outRead, &outWrite, nullptr, 0))
    {
        if (err)
            *err = LastErrorText(L"CreatePipe");
        return false;
    }

    const COORD size{ cols > 0 ? cols : (SHORT)80, rows > 0 ? rows : (SHORT)25 };
    HRESULT hr = CreatePseudoConsole(size, inRead, outWrite, 0, &m_hPC);

    // ConPTY duplicates both handles, so our copies of the far ends go now.
    CloseIfValid(inRead);
    CloseIfValid(outWrite);

    if (FAILED(hr))
    {
        if (err)
        {
            wchar_t hex[16];
            swprintf_s(hex, L"%08lX", static_cast<unsigned long>(hr));
            *err = std::wstring(L"CreatePseudoConsole failed (0x") + hex +
                   L"). ConPTY needs Windows 10 1809 or newer.";
        }
        return false;
    }

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    m_attrs = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, attrSize));
    if (!m_attrs || !InitializeProcThreadAttributeList(m_attrs, 1, 0, &attrSize))
    {
        if (err)
            *err = LastErrorText(L"InitializeProcThreadAttributeList");
        return false;
    }

    if (!UpdateProcThreadAttribute(m_attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   m_hPC, sizeof(m_hPC), nullptr, nullptr))
    {
        if (err)
            *err = LastErrorText(L"UpdateProcThreadAttribute");
        return false;
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    si.lpAttributeList = m_attrs;

    std::wstring mutableCmd = commandLine;   // CreateProcessW writes to this
    mutableCmd.push_back(L'\0');

    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                        nullptr, nullptr, &si.StartupInfo, &m_pi))
    {
        if (err)
            *err = LastErrorText(L"CreateProcess");
        return false;
    }

    m_running.store(true, std::memory_order_release);
    m_reader = std::thread(&ConPty::ReaderLoop, this);
    return true;
}

void ConPty::ReaderLoop()
{
    std::vector<char> buf(16 * 1024);
    for (;;)
    {
        DWORD read = 0;
        const BOOL ok = ReadFile(m_outRead, buf.data(), static_cast<DWORD>(buf.size()),
                                 &read, nullptr);
        if (!ok || read == 0)
            break;                       // pty closed, or the child is gone

        if (m_onOutput)
            m_onOutput(std::vector<char>(buf.begin(), buf.begin() + read));
    }

    m_running.store(false, std::memory_order_release);

    DWORD code = 0;
    if (m_pi.hProcess)
        GetExitCodeProcess(m_pi.hProcess, &code);
    if (m_onExit)
        m_onExit(code);
}

void ConPty::Write(const char* data, size_t len)
{
    if (!data || len == 0)
        return;

    std::lock_guard<std::mutex> guard(m_writeLock);
    if (m_inWrite == INVALID_HANDLE_VALUE)
        return;

    size_t off = 0;
    while (off < len)
    {
        DWORD wrote = 0;
        if (!WriteFile(m_inWrite, data + off, static_cast<DWORD>(len - off), &wrote, nullptr) ||
            wrote == 0)
            break;
        off += wrote;
    }
}

void ConPty::Resize(SHORT cols, SHORT rows)
{
    if (m_hPC && cols > 0 && rows > 0)
        ResizePseudoConsole(m_hPC, COORD{ cols, rows });
}

void ConPty::Shutdown()
{
    // Idempotent, and safe to call after a half-completed Start().
    //
    // 1. EOF on stdin asks pwsh to leave of its own accord.
    {
        std::lock_guard<std::mutex> guard(m_writeLock);
        CloseIfValid(m_inWrite);
    }

    if (m_pi.hProcess)
    {
        if (WaitForSingleObject(m_pi.hProcess, 2000) != WAIT_OBJECT_0)
            TerminateProcess(m_pi.hProcess, 0);
        WaitForSingleObject(m_pi.hProcess, 2000);
    }

    // 2. ClosePseudoConsole blocks until the output pipe has drained, so the
    //    reader thread has to still be running here. Joining first deadlocks.
    if (m_hPC)
    {
        ClosePseudoConsole(m_hPC);
        m_hPC = nullptr;
    }

    if (m_reader.joinable())
        m_reader.join();

    CloseIfValid(m_outRead);

    if (m_pi.hThread)
    {
        CloseHandle(m_pi.hThread);
        m_pi.hThread = nullptr;
    }
    if (m_pi.hProcess)
    {
        CloseHandle(m_pi.hProcess);
        m_pi.hProcess = nullptr;
    }

    if (m_attrs)
    {
        DeleteProcThreadAttributeList(m_attrs);
        HeapFree(GetProcessHeap(), 0, m_attrs);
        m_attrs = nullptr;
    }

    m_running.store(false, std::memory_order_release);
}
