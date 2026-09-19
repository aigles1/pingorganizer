#pragma once

#include <wx/wx.h>

#include <wrl.h>
#include <WebView2.h>

#include <functional>
#include <string>

#include "ConPty.h"

// A wxPanel whose whole client area is a WebView2 hosting xterm.js, wired to a
// ConPTY running pwsh.exe. From PowerShell's point of view this is an ordinary
// terminal, so PSReadLine, tab completion, colours and Ctrl+C all work.
class TerminalPanel : public wxPanel
{
public:
    explicit TerminalPanel(wxWindow* parent, wxWindowID id = wxID_ANY);
    ~TerminalPanel() override;

    // Types text into the shell. Include a trailing "\r" to submit it.
    void SendText(const std::string& utf8);

    void FocusTerminal();
    bool ShellRunning() const { return m_pty.Running(); }

    // Optional sink for one-line progress/error text (wired to the status bar).
    std::function<void(const wxString&)> OnStatus;

private:
    void InitWebView();
    void OnWebMessage(const std::wstring& msg);
    void StartShell(SHORT cols, SHORT rows);
    void PostToPage(const std::wstring& msg);
    void LayoutWebView();
    void Status(const wxString& text);
    void Fail(const wxString& text);

    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_webview;

    ConPty m_pty;
    bool m_pageReady = false;
    bool m_shellStarted = false;
    std::string m_pending;              // typed before the shell came up

    wxStaticText* m_placeholder = nullptr;
};
