#include "TerminalPanel.h"

#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include "Base64.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace
{
    // Any hostname works as long as it never resolves publicly; ".invalid" is
    // reserved by RFC 2606 for exactly this.
    constexpr wchar_t kVirtualHost[] = L"psping.invalid";
    constexpr wchar_t kStartPage[] = L"https://psping.invalid/terminal.html";

    wxString AssetsDir()
    {
        wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
        exe.SetFullName("");
        exe.AppendDir("assets");
        return exe.GetPathWithSep();
    }

    wxString Env(const wxString& name)
    {
        wxString value;
        wxGetEnv(name, &value);
        return value;
    }

    std::wstring Widen(const std::string& ascii)
    {
        return std::wstring(ascii.begin(), ascii.end());
    }

    // Locates pwsh.exe: PATH first, then the standard install locations, so the
    // app still works when PowerShell 7 was installed for the current user only.
    wxString FindPwsh()
    {
        wxPathList paths;
        paths.AddEnvList("PATH");
        wxString hit = paths.FindAbsoluteValidPath("pwsh.exe");
        if (!hit.empty())
            return hit;

        const wxString candidates[] = {
            Env("ProgramFiles") + "\\PowerShell\\7\\pwsh.exe",
            Env("ProgramW6432") + "\\PowerShell\\7\\pwsh.exe",
            Env("LOCALAPPDATA") + "\\Microsoft\\WindowsApps\\pwsh.exe",
        };
        for (const wxString& c : candidates)
            if (wxFileName::FileExists(c))
                return c;

        return wxEmptyString;
    }
}

TerminalPanel::TerminalPanel(wxWindow* parent, wxWindowID id)
    : wxPanel(parent, id, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE)
{
    SetBackgroundColour(wxColour(0x0C, 0x0C, 0x0C));

    m_placeholder = new wxStaticText(this, wxID_ANY, "Starting PowerShell 7…",
                                     wxPoint(12, 10));
    m_placeholder->SetForegroundColour(wxColour(0x99, 0x99, 0x99));
    m_placeholder->SetBackgroundColour(wxColour(0x0C, 0x0C, 0x0C));

    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { LayoutWebView(); e.Skip(); });
    Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) { FocusTerminal(); e.Skip(); });

    InitWebView();
}

TerminalPanel::~TerminalPanel()
{
    // Order matters: stop the shell (which joins the reader thread) before the
    // WebView2 controller goes away, so no output can be posted to a dead page.
    m_pty.Shutdown();
    if (m_controller)
        m_controller->Close();
}

void TerminalPanel::Status(const wxString& text)
{
    if (OnStatus)
        OnStatus(text);
}

void TerminalPanel::Fail(const wxString& text)
{
    if (m_placeholder)
    {
        m_placeholder->Show();
        m_placeholder->SetForegroundColour(wxColour(0xE7, 0x48, 0x56));
        m_placeholder->SetLabel(text);
        m_placeholder->Wrap(wxMax(200, GetClientSize().GetWidth() - 24));
    }
    Status(text);
}

void TerminalPanel::InitWebView()
{
    wxFileName udf(wxStandardPaths::Get().GetUserLocalDataDir(), "");
    udf.AppendDir("WebView2");
    const std::wstring userData = udf.GetPath().ToStdWstring();

    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT
            {
                if (FAILED(result) || !env)
                {
                    Fail("WebView2 runtime not available. Install the Evergreen "
                         "WebView2 Runtime from Microsoft, then restart.");
                    return S_OK;
                }

                env->CreateCoreWebView2Controller(
                    static_cast<HWND>(GetHWND()),
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT r, ICoreWebView2Controller* controller) -> HRESULT
                        {
                            if (FAILED(r) || !controller)
                            {
                                Fail("Could not create the WebView2 control.");
                                return S_OK;
                            }

                            m_controller = controller;
                            m_controller->get_CoreWebView2(&m_webview);

                            ComPtr<ICoreWebView2Controller2> c2;
                            if (SUCCEEDED(m_controller.As(&c2)))
                                c2->put_DefaultBackgroundColor(
                                    COREWEBVIEW2_COLOR{ 255, 0x0C, 0x0C, 0x0C });

                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(m_webview->get_Settings(&settings)))
                            {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                                settings->put_IsBuiltInErrorPageEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(TRUE);

                                // Critical: without this the browser swallows
                                // Ctrl+F, Ctrl+P, F5 etc. instead of the shell.
                                ComPtr<ICoreWebView2Settings3> s3;
                                if (SUCCEEDED(settings.As(&s3)))
                                    s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);

                                ComPtr<ICoreWebView2Settings4> s4;
                                if (SUCCEEDED(settings.As(&s4)))
                                {
                                    s4->put_IsGeneralAutofillEnabled(FALSE);
                                    s4->put_IsPasswordAutosaveEnabled(FALSE);
                                }

                                ComPtr<ICoreWebView2Settings6> s6;
                                if (SUCCEEDED(settings.As(&s6)))
                                    s6->put_IsSwipeNavigationEnabled(FALSE);
                            }

                            // Serve ./assets as an https origin. file:// would
                            // trip the CORS rules and the page CSP.
                            ComPtr<ICoreWebView2_3> wv3;
                            if (SUCCEEDED(m_webview.As(&wv3)))
                            {
                                const std::wstring dir = AssetsDir().ToStdWstring();
                                wv3->SetVirtualHostNameToFolderMapping(
                                    kVirtualHost, dir.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                            }

                            EventRegistrationToken token{};
                            m_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](ICoreWebView2*,
                                           ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
                                    {
                                        LPWSTR raw = nullptr;
                                        if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw)
                                        {
                                            std::wstring msg(raw);
                                            CoTaskMemFree(raw);
                                            OnWebMessage(msg);
                                        }
                                        return S_OK;
                                    })
                                    .Get(),
                                &token);

                            // Right-click paste needs clipboard read. Grant that
                            // to our own page and refuse everything else.
                            m_webview->add_PermissionRequested(
                                Callback<ICoreWebView2PermissionRequestedEventHandler>(
                                    [](ICoreWebView2*,
                                       ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT
                                    {
                                        COREWEBVIEW2_PERMISSION_KIND kind =
                                            COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
                                        args->get_PermissionKind(&kind);
                                        args->put_State(
                                            kind == COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ
                                                ? COREWEBVIEW2_PERMISSION_STATE_ALLOW
                                                : COREWEBVIEW2_PERMISSION_STATE_DENY);
                                        return S_OK;
                                    })
                                    .Get(),
                                &token);

                            LayoutWebView();
                            m_webview->Navigate(kStartPage);
                            return S_OK;
                        })
                        .Get());
                return S_OK;
            })
            .Get());

    if (FAILED(hr))
        Fail("WebView2 runtime not found. Install the Evergreen WebView2 Runtime "
             "from Microsoft, then restart.");
}

void TerminalPanel::LayoutWebView()
{
    if (!m_controller)
        return;
    const wxSize sz = GetClientSize();
    m_controller->put_Bounds(RECT{ 0, 0, sz.GetWidth(), sz.GetHeight() });
}

void TerminalPanel::OnWebMessage(const std::wstring& msg)
{
    if (msg.rfind(L"i:", 0) == 0)
    {
        const std::string b64(msg.begin() + 2, msg.end());
        const std::vector<char> bytes = base64::Decode(b64);
        m_pty.Write(bytes.data(), bytes.size());
        return;
    }

    if (msg.rfind(L"s:", 0) == 0)
    {
        int cols = 0, rows = 0;
        if (swscanf_s(msg.c_str() + 2, L"%d,%d", &cols, &rows) == 2)
            m_pty.Resize(static_cast<SHORT>(cols), static_cast<SHORT>(rows));
        return;
    }

    if (msg.rfind(L"ready:", 0) == 0)
    {
        int cols = 80, rows = 25;
        swscanf_s(msg.c_str() + 6, L"%d,%d", &cols, &rows);
        m_pageReady = true;
        if (m_placeholder)
            m_placeholder->Hide();
        StartShell(static_cast<SHORT>(cols), static_cast<SHORT>(rows));
    }
}

void TerminalPanel::StartShell(SHORT cols, SHORT rows)
{
    if (m_shellStarted)
        return;
    m_shellStarted = true;

    const wxString pwsh = FindPwsh();
    if (pwsh.empty())
    {
        PostToPage(L"x:pwsh.exe was not found. Install PowerShell 7, then restart.");
        Status("PowerShell 7 (pwsh.exe) not found.");
        return;
    }

    const std::wstring cmd = L"\"" + pwsh.ToStdWstring() + L"\" -NoLogo";

    std::wstring err;
    const bool ok = m_pty.Start(
        cmd, cols, rows,
        // Reader thread. CallAfter must stay asynchronous or ConPty::Shutdown
        // would deadlock waiting on a thread that is waiting on the GUI.
        [this](std::vector<char> bytes)
        {
            std::string b64 = base64::Encode(bytes.data(), bytes.size());
            CallAfter([this, b64 = std::move(b64)] { PostToPage(L"o:" + Widen(b64)); });
        },
        [this](DWORD code)
        {
            CallAfter(
                [this, code]
                {
                    PostToPage(
                        wxString::Format("x:[pwsh exited with code %lu]", code).ToStdWstring());
                    Status(wxString::Format("PowerShell exited (code %lu).", code));
                });
        },
        &err);

    if (!ok)
    {
        PostToPage(L"x:" + err);
        Status(wxString(err));
        return;
    }

    Status("PowerShell 7 ready.");

    if (!m_pending.empty())
    {
        m_pty.Write(m_pending.data(), m_pending.size());
        m_pending.clear();
    }
}

void TerminalPanel::PostToPage(const std::wstring& msg)
{
    if (m_webview && m_pageReady)
        m_webview->PostWebMessageAsString(msg.c_str());
}

void TerminalPanel::SendText(const std::string& utf8)
{
    if (m_pty.Running())
        m_pty.Write(utf8.data(), utf8.size());
    else
        m_pending += utf8;   // replayed once the shell is up
}

void TerminalPanel::FocusTerminal()
{
    if (m_controller)
        m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    PostToPage(L"focus");
}
