#include <wx/wx.h>
#include <wx/filename.h>
#include <wx/hyperlink.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/statline.h>
#include <wx/stdpaths.h>
#include <wx/textfile.h>

#include <windows.h>

#include <string>
#include <vector>

#include "TerminalPanel.h"

namespace
{
    // Values offered by the Count dropdown. The selection index is looked up in
    // here rather than treated as the count, so the list need not be contiguous.
    constexpr int kCountChoices[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 50, 100 };

    // Test-Connection defaults to 4 echo requests, so start the dropdown there.
    constexpr int kDefaultCount = 4;

    // Upper bound on generated rows, so a very tall screen cannot spin the
    // fill-to-bottom loop into thousands of controls.
    constexpr int kMaxPresetRows = 64;

    const char* const kProjectUrl = "https://github.com/aigles1/pingorganizer";

    enum
    {
        ID_OpenPresets = wxID_HIGHEST + 1,
    };

    // Written to presets.txt on first run, and used if the file is unreadable or
    // has no usable lines. The file is the source of truth after that.
    const wxString kDefaultPresets[] = {
        "127.0.0.1",
        "speedtest.net",
        "9.9.9.10",
        "1.1.1.1",
        "1.0.0.1",
        "1.1.1.2",
        "1.1.1.3",
        "8.8.4.4",
        "94.140.14.140",
        "8.26.56.26",
        "76.76.2.1",
    };

    // %APPDATA%\PSPingGui\presets.txt - deliberately not next to the exe, which
    // build.ps1 -Clean deletes wholesale.
    wxFileName PresetsPath()
    {
        return wxFileName(wxStandardPaths::Get().GetUserDataDir(), "presets.txt");
    }

    void SeedPresetsFile(const wxFileName& path)
    {
        if (!wxFileName::DirExists(path.GetPath()))
            wxFileName::Mkdir(path.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

        wxTextFile file(path.GetFullPath());
        if (!file.Create())
            return;

        file.AddLine("# PSPingGui presets - one target per line.");
        file.AddLine("# Blank lines and lines starting with # are ignored.");
        file.AddLine("# Edit freely, then restart the app.");
        file.AddLine(wxEmptyString);
        for (const wxString& preset : kDefaultPresets)
            file.AddLine(preset);
        file.Write();
    }

    std::vector<wxString> LoadPresets()
    {
        std::vector<wxString> out;

        const wxFileName path = PresetsPath();
        if (!path.FileExists())
            SeedPresetsFile(path);

        wxTextFile file(path.GetFullPath());
        if (file.Open())
        {
            for (size_t i = 0; i < file.GetLineCount(); ++i)
            {
                wxString line = file[i];
                line.Trim(true).Trim(false);
                if (line.empty() || line.StartsWith("#"))
                    continue;
                out.push_back(line);
            }
            file.Close();
        }

        if (out.empty())
            out.assign(std::begin(kDefaultPresets), std::end(kDefaultPresets));

        return out;
    }

    // -----------------------------------------------------------------------
    // Menu bar painting.
    //
    // Windows 11 paints the menu bar itself, in COLOR_WINDOW white, and there
    // is no supported way to change that: wxMenuBar::SetBackgroundColour is a
    // no-op on MSW, and SetMenuInfo's MIM_BACKGROUND brush reaches popup menus
    // but not the bar. Both were tried and measured.
    //
    // What follows is the "UAH" technique dark-mode applications use. Before
    // painting the bar, Windows sends these undocumented messages; answering
    // them lets the application draw it instead. They are absent from the SDK,
    // so the structures are declared here to match what the OS passes.
    //
    // This is unsupported by definition. If a future Windows stops sending the
    // messages, the handlers simply never run and the bar reverts to its
    // default white - the app keeps working.
    // -----------------------------------------------------------------------
    constexpr UINT kWmUahDrawMenu = 0x0091;
    constexpr UINT kWmUahDrawMenuItem = 0x0092;

    struct UahMenu
    {
        HMENU hmenu;
        HDC hdc;
        DWORD dwFlags;
    };

    union UahMenuItemMetrics
    {
        struct { DWORD cx; DWORD cy; } rgsizeBar[2];
        struct { DWORD cx; DWORD cy; } rgsizePopup[4];
    };

    struct UahMenuPopupMetrics
    {
        DWORD rgcx[4];
        DWORD fUpdateMaxWidths : 2;
    };

    struct UahMenuItem
    {
        int iPosition;
        UahMenuItemMetrics umim;
        UahMenuPopupMetrics umpm;
    };

    struct UahDrawMenuItem
    {
        DRAWITEMSTRUCT dis;
        UahMenu um;
        UahMenuItem umi;
    };

    // The bar is painted in the same colour the frame uses, which is the whole
    // point of the exercise.
    COLORREF MenuBarColour()
    {
        const wxColour face = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
        return RGB(face.Red(), face.Green(), face.Blue());
    }

    COLORREF Shade(COLORREF colour, int delta)
    {
        const auto clamp = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
        return RGB(clamp(GetRValue(colour) + delta),
                   clamp(GetGValue(colour) + delta),
                   clamp(GetBValue(colour) + delta));
    }

    void FillWith(HDC hdc, const RECT& rect, COLORREF colour)
    {
        HBRUSH brush = ::CreateSolidBrush(colour);
        ::FillRect(hdc, &rect, brush);
        ::DeleteObject(brush);
    }

    // A PowerShell single-quoted string has exactly one escape: two quotes for a
    // literal quote. Doubling those, plus dropping control characters (which would
    // let a pasted newline submit a second command), makes any input safe to embed.
    wxString QuoteForPowerShell(const wxString& raw)
    {
        wxString out;
        out.reserve(raw.length() + 8);
        for (const wxUniChar ch : raw)
        {
            if (ch < 0x20 || ch == 0x7F)
                continue;
            if (ch == '\'')
                out += '\'';
            out += ch;
        }
        return out;
    }
}

class MainFrame : public wxFrame
{
public:
    MainFrame();

protected:
    // Intercepts the undocumented menu-bar painting messages; everything else
    // goes straight on to wxWidgets.
    WXLRESULT MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam) override;

private:
    bool DrawMenuBarBackground(WXLPARAM lParam);
    bool DrawMenuBarItem(WXLPARAM lParam);
    void PaintMenuBarBottomLine();

    void OnRun(wxCommandEvent& event);
    void OnOpenPresets(wxCommandEvent& event);
    void OnExit(wxCommandEvent& event);
    void OnAbout(wxCommandEvent& event);
    void OnPresetAreaSize(wxSizeEvent& event);

    // Every entry point funnels here: the top bar, each preset row, and Enter in
    // any of their textboxes. `source` is the box to put focus back into when the
    // target is blank.
    void RunFor(const wxString& raw, wxTextCtrl* source);

    void BuildMenuBar();
    wxWindow* BuildPresets(wxWindow* parent);
    wxButton* MakeRunButton(wxWindow* parent, wxWindowID id);
    void AddPresetRow(const wxString& target);
    void EnsureRowsFill();

    wxTextCtrl* m_target = nullptr;
    wxChoice* m_count = nullptr;
    TerminalPanel* m_terminal = nullptr;

    wxScrolledWindow* m_presetArea = nullptr;
    wxBoxSizer* m_presetSizer = nullptr;
    wxFlexGridSizer* m_presetGrid = nullptr;
    int m_rowCount = 0;
    int m_rowStep = 0;      // height of one row plus the gap below it
    bool m_fillingRows = false;
};

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "Test-Connection", wxDefaultPosition, wxSize(1180, 700))
{
    SetMinSize(wxSize(780, 320));

    BuildMenuBar();

    // Controls sit straight on the frame rather than on a wxPanel, so the frame
    // supplies their background. The menu bar above stays white whatever is set
    // here - Windows 11 paints it itself - see BuildMenuBar.
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));

    m_target = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                              wxDefaultSize, wxTE_PROCESS_ENTER);
    m_target->SetHint("IP address or hostname");

    wxButton* run = MakeRunButton(this, wxID_OK);

    wxStaticText* countLabel = new wxStaticText(this, wxID_ANY, "Count");
    m_count = new wxChoice(this, wxID_ANY);
    for (const int n : kCountChoices)
    {
        m_count->Append(wxString::Format("%d", n));
        if (n == kDefaultCount)
            m_count->SetSelection(m_count->GetCount() - 1);
    }

    m_terminal = new TerminalPanel(this);
    m_terminal->OnStatus = [this](const wxString& text) { SetStatusText(text); };

    const int vcentre = wxALIGN_CENTRE_VERTICAL;
    wxBoxSizer* rightBar = new wxBoxSizer(wxHORIZONTAL);
    rightBar->Add(run, 0, vcentre | wxRIGHT, 12);
    rightBar->Add(countLabel, 0, vcentre | wxRIGHT, 6);
    rightBar->Add(m_count, 0, vcentre);

    // One grid for both rows, so the target box and the terminal share a column
    // and are therefore exactly the same width by construction - no matching of
    // border values that would drift the moment either row changes.
    wxFlexGridSizer* grid = new wxFlexGridSizer(3, 0, 0);
    grid->AddGrowableCol(0, 1);     // terminal column takes all the slack
    grid->AddGrowableRow(1, 1);

    // Row 0: the target box, with no side border at all - any would make it
    // narrower than the terminal beneath it.
    grid->Add(m_target, 0, wxEXPAND | wxTOP | wxBOTTOM, 8);
    grid->AddSpacer(0);
    grid->Add(rightBar, 0, wxEXPAND | wxTOP | wxBOTTOM | wxLEFT, 8);

    // Row 1: terminal, divider, presets. Column 1 exists only to hold the
    // divider, which is what gives row 0 its matching gap.
    grid->Add(m_terminal, 1, wxEXPAND);
    grid->Add(new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxLI_VERTICAL), 0, wxEXPAND);
    grid->Add(BuildPresets(this), 0, wxEXPAND);

    SetSizer(grid);

    CreateStatusBar();
    SetStatusText("Starting PowerShell 7...");

    Bind(wxEVT_BUTTON, &MainFrame::OnRun, this, wxID_OK);
    m_target->Bind(wxEVT_TEXT_ENTER, &MainFrame::OnRun, this);

    run->SetDefault();
    m_target->SetFocus();
}

WXLRESULT MainFrame::MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam)
{
    switch (nMsg)
    {
        case kWmUahDrawMenu:
            if (DrawMenuBarBackground(lParam))
                return 1;
            break;

        case kWmUahDrawMenuItem:
            if (DrawMenuBarItem(lParam))
                return 1;
            break;

        // Windows draws a light hairline between the bar and the client area
        // during non-client painting, after which it is ours to cover.
        case WM_NCACTIVATE:
        case WM_NCPAINT:
        {
            const WXLRESULT result =
                wxFrame::MSWWindowProc(nMsg, wParam, lParam);
            PaintMenuBarBottomLine();
            return result;
        }
    }

    return wxFrame::MSWWindowProc(nMsg, wParam, lParam);
}

bool MainFrame::DrawMenuBarBackground(WXLPARAM lParam)
{
    auto* menu = reinterpret_cast<UahMenu*>(lParam);
    if (!menu || !menu->hdc)
        return false;

    HWND hwnd = static_cast<HWND>(GetHandle());

    MENUBARINFO info = {};
    info.cbSize = sizeof(info);
    if (!::GetMenuBarInfo(hwnd, OBJID_MENU, 0, &info))
        return false;

    RECT window = {};
    ::GetWindowRect(hwnd, &window);

    // rcBar is in screen coordinates, and the device context is window-relative.
    RECT bar = info.rcBar;
    ::OffsetRect(&bar, -window.left, -window.top);
    bar.top -= 1;       // rcBar reports one pixel short of what is painted

    FillWith(menu->hdc, bar, MenuBarColour());
    return true;
}

bool MainFrame::DrawMenuBarItem(WXLPARAM lParam)
{
    auto* item = reinterpret_cast<UahDrawMenuItem*>(lParam);
    if (!item || !item->um.hdc)
        return false;

    wchar_t text[256] = {};
    MENUITEMINFOW mii = {};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STRING;
    mii.dwTypeData = text;
    mii.cch = static_cast<UINT>(std::size(text) - 1);
    if (!::GetMenuItemInfoW(item->um.hmenu, item->umi.iPosition, TRUE, &mii))
        return false;

    const COLORREF face = MenuBarColour();
    COLORREF back = face;
    COLORREF fore = ::GetSysColor(COLOR_MENUTEXT);

    if (item->dis.itemState & ODS_HOTLIGHT)
        back = Shade(face, -16);
    if (item->dis.itemState & ODS_SELECTED)      // its popup is open
        back = Shade(face, -30);
    if (item->dis.itemState & (ODS_GRAYED | ODS_DISABLED))
        fore = ::GetSysColor(COLOR_GRAYTEXT);

    UINT flags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
    if (item->dis.itemState & ODS_NOACCEL)
        flags |= DT_HIDEPREFIX;                  // Alt not held: hide the & rule

    FillWith(item->um.hdc, item->dis.rcItem, back);
    ::SetBkMode(item->um.hdc, TRANSPARENT);
    ::SetTextColor(item->um.hdc, fore);
    ::DrawTextW(item->um.hdc, text, -1, &item->dis.rcItem, flags);
    return true;
}

void MainFrame::PaintMenuBarBottomLine()
{
    HWND hwnd = static_cast<HWND>(GetHandle());

    MENUBARINFO info = {};
    info.cbSize = sizeof(info);
    if (!::GetMenuBarInfo(hwnd, OBJID_MENU, 0, &info))
        return;

    RECT client = {};
    ::GetClientRect(hwnd, &client);
    ::MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&client), 2);

    RECT window = {};
    ::GetWindowRect(hwnd, &window);
    ::OffsetRect(&client, -window.left, -window.top);

    RECT line = client;
    line.bottom = line.top;
    line.top -= 1;

    HDC hdc = ::GetWindowDC(hwnd);
    if (!hdc)
        return;
    FillWith(hdc, line, MenuBarColour());
    ::ReleaseDC(hwnd, hdc);
}

// U+25B6 BLACK RIGHT-POINTING TRIANGLE, built from the code point so the label
// does not depend on how this file happens to be encoded. A column of identical
// glyphs reads as texture and lets the eye go straight to the addresses; a
// column of identical words has to be re-read every pass.
wxButton* MainFrame::MakeRunButton(wxWindow* parent, wxWindowID id)
{
    const wxString runGlyph(wxUniChar(0x25B6));

    wxButton* button = new wxButton(parent, id, runGlyph, wxDefaultPosition,
                                    wxSize(34, -1));

    // A glyph carries no name for assistive tech, so give the button one both
    // ways. The tooltip stays generic because the box beside it is editable.
    button->SetToolTip("Ping this target");
    button->SetName("Ping this target");

    return button;
}

void MainFrame::BuildMenuBar()
{
    wxMenu* fileMenu = new wxMenu;
    fileMenu->Append(ID_OpenPresets, "&Presets");
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_EXIT, "E&xit\tAlt+F4");

    wxMenu* helpMenu = new wxMenu;
    helpMenu->Append(wxID_ABOUT, "&About\tF1");

    wxMenuBar* menus = new wxMenuBar;
    menus->Append(fileMenu, "&File");
    menus->Append(helpMenu, "&Help");
    SetMenuBar(menus);

    // The bar itself is painted grey in MSWWindowProc - see the long note on
    // the UAH messages near the top of this file.

    Bind(wxEVT_MENU, &MainFrame::OnOpenPresets, this, ID_OpenPresets);
    Bind(wxEVT_MENU, &MainFrame::OnExit, this, wxID_EXIT);
    Bind(wxEVT_MENU, &MainFrame::OnAbout, this, wxID_ABOUT);
}

wxWindow* MainFrame::BuildPresets(wxWindow* parent)
{
    m_presetArea = new wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition,
                                        wxDefaultSize, wxVSCROLL);
    m_presetArea->SetScrollRate(0, 8);
    m_presetArea->SetMinSize(wxSize(250, -1));
    m_presetArea->SetBackgroundColour(
        wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));

    wxStaticText* heading = new wxStaticText(m_presetArea, wxID_ANY, "Presets");
    heading->SetToolTip(PresetsPath().GetFullPath() +
                        "\n\nFile > Presets opens this file."
                        "\nRestart the app to apply changes.");

    // Two columns - textbox, button - one row per preset.
    m_presetGrid = new wxFlexGridSizer(2, 6, 8);
    m_presetGrid->AddGrowableCol(0, 1);

    for (const wxString& preset : LoadPresets())
        AddPresetRow(preset);

    wxBoxSizer* inner = new wxBoxSizer(wxVERTICAL);
    inner->Add(heading, 0, wxBOTTOM, 6);
    inner->Add(m_presetGrid, 0, wxEXPAND);

    m_presetSizer = new wxBoxSizer(wxVERTICAL);
    m_presetSizer->Add(inner, 0, wxEXPAND | wxALL, 10);
    m_presetArea->SetSizer(m_presetSizer);

    m_presetArea->Bind(wxEVT_SIZE, &MainFrame::OnPresetAreaSize, this);
    return m_presetArea;
}

void MainFrame::AddPresetRow(const wxString& target)
{
    wxTextCtrl* box = new wxTextCtrl(m_presetArea, wxID_ANY, target, wxDefaultPosition,
                                     wxDefaultSize, wxTE_PROCESS_ENTER);
    box->SetMinSize(wxSize(150, -1));

    if (m_rowStep <= 0)
        m_rowStep = box->GetBestSize().y + m_presetGrid->GetVGap();

    // Deliberately not wxID_OK: the frame-level handler claims that id, and
    // these rows carry their own target.
    wxButton* go = MakeRunButton(m_presetArea, wxID_ANY);

    auto run = [this, box](wxCommandEvent&) { RunFor(box->GetValue(), box); };
    go->Bind(wxEVT_BUTTON, run);
    box->Bind(wxEVT_TEXT_ENTER, run);

    m_presetGrid->Add(box, 1, wxEXPAND);
    m_presetGrid->Add(go, 0, wxALIGN_CENTRE_VERTICAL);
    ++m_rowCount;
}

// Grow the column with blank rows until it reaches the bottom of the window.
// Rows are only ever added, so text typed into a spare row survives a resize.
void MainFrame::EnsureRowsFill()
{
    if (!m_presetArea || m_fillingRows)
        return;

    m_fillingRows = true;
    const int available = m_presetArea->GetClientSize().y;
    const int step = m_rowStep > 0 ? m_rowStep : 1;

    // Look one row ahead, so the column stops flush with the bottom instead of
    // overshooting it and raising a scrollbar nothing needs to scroll.
    while (m_rowCount < kMaxPresetRows &&
           m_presetSizer->CalcMin().y + step <= available)
        AddPresetRow(wxEmptyString);

    m_presetArea->Layout();
    m_presetArea->FitInside();
    m_fillingRows = false;
}

void MainFrame::OnPresetAreaSize(wxSizeEvent& event)
{
    EnsureRowsFill();
    event.Skip();
}

void MainFrame::OnOpenPresets(wxCommandEvent& WXUNUSED(event))
{
    const wxFileName path = PresetsPath();

    // Self-healing: the file may have been deleted since startup, and an "open"
    // that silently does nothing is worse than one that hands back the defaults.
    if (!path.FileExists())
        SeedPresetsFile(path);

    if (wxLaunchDefaultApplication(path.GetFullPath()))
        SetStatusText(path.GetFullPath() + "  (restart to apply changes)");
    else
        SetStatusText("Could not open " + path.GetFullPath());
}

void MainFrame::OnExit(wxCommandEvent& WXUNUSED(event))
{
    Close(true);
}

void MainFrame::OnAbout(wxCommandEvent& WXUNUSED(event))
{
    wxDialog dlg(this, wxID_ANY, "About PSPingGui");

    wxStaticText* name = new wxStaticText(&dlg, wxID_ANY, "PSPingGui");
    wxFont nameFont = name->GetFont();
    nameFont.MakeBold().MakeLarger();
    name->SetFont(nameFont);

    wxStaticText* blurb = new wxStaticText(&dlg, wxID_ANY,
        "A Test-Connection front end with a real embedded PowerShell 7 terminal.");

    wxHyperlinkCtrl* link =
        new wxHyperlinkCtrl(&dlg, wxID_ANY, kProjectUrl, kProjectUrl);

    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(name, 0, wxLEFT | wxRIGHT | wxTOP, 16);
    sizer->Add(blurb, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    sizer->Add(link, 0, wxLEFT | wxRIGHT | wxTOP, 12);
    if (wxSizer* buttons = dlg.CreateButtonSizer(wxOK))
        sizer->Add(buttons, 0, wxEXPAND | wxALL, 16);

    dlg.SetSizerAndFit(sizer);
    dlg.CentreOnParent();
    dlg.ShowModal();
}

void MainFrame::OnRun(wxCommandEvent& WXUNUSED(event))
{
    RunFor(m_target->GetValue(), m_target);
}

void MainFrame::RunFor(const wxString& raw, wxTextCtrl* source)
{
    // Trim mutates in place and returns a reference, so it needs a copy of its own.
    wxString target = raw;
    target.Trim(true).Trim(false);
    if (target.empty())
    {
        SetStatusText("Enter an IP address or hostname first.");
        source->SetFocus();
        return;
    }

    const int sel = m_count->GetSelection();
    const long count = (sel >= 0 && sel < (int)std::size(kCountChoices))
                           ? kCountChoices[sel]
                           : kDefaultCount;
    const wxString command =
        wxString::Format("Test-Connection -TargetName '%s' -Count %ld",
                         QuoteForPowerShell(target), count);

    // Written to the pty exactly as if it had been typed, so PSReadLine shows it
    // on the prompt line and it lands in the shell's own history.
    m_terminal->SendText(std::string(command.utf8_string()) + "\r");
    m_terminal->FocusTerminal();

    SetStatusText(command);
}

class PSPingApp : public wxApp
{
public:
    bool OnInit() override
    {
        if (!wxApp::OnInit())
            return false;

        SetAppName("PSPingGui");
        (new MainFrame())->Show();
        return true;
    }
};

wxIMPLEMENT_APP(PSPingApp);
