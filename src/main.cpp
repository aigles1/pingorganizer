#include <wx/wx.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/hyperlink.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/stdpaths.h>
#include <wx/textfile.h>

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <set>
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
        ID_ToggleDarkMode,
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

    // %APPDATA%\PingOrganizer\presets.txt - deliberately not next to the exe, which
    // build.ps1 -Clean deletes wholesale.
    wxFileName PresetsPath()
    {
        return wxFileName(wxStandardPaths::Get().GetUserDataDir(), "presets.txt");
    }

    // Releases up to v0.2 were called PSPingGui, and the app name decides the
    // config folder, so their presets live in %APPDATA%\PSPingGui. Carry an
    // existing file across the first time this build runs. Copy, not move, so
    // an older build pointed at the old folder keeps working too.
    void MigrateLegacyPresets(const wxFileName& path)
    {
        if (path.FileExists())
            return;

        wxFileName legacy(path);
        legacy.RemoveLastDir();
        legacy.AppendDir("PSPingGui");
        if (!legacy.FileExists())
            return;

        if (!wxFileName::DirExists(path.GetPath()))
            wxFileName::Mkdir(path.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        wxCopyFile(legacy.GetFullPath(), path.GetFullPath(), false);
    }

    // presets.txt also carries settings, as "key = value" lines. A line holding
    // '=' is always a setting and never a target: no hostname or IP address,
    // v4 or v6, can contain one. Keys and values are case-insensitive.
    bool ParseSetting(const wxString& line, wxString& key, wxString& value)
    {
        const int eq = line.Find('=');
        if (eq == wxNOT_FOUND)
            return false;

        key = line.Left(eq);
        key.Trim(true).Trim(false).MakeLower();
        value = line.Mid(eq + 1);
        value.Trim(true).Trim(false).MakeLower();
        return true;
    }

    const char* const kAppearanceKey = "appearance";

    wxString AppearanceLine(bool dark)
    {
        return wxString::Format("%s = %s", kAppearanceKey, dark ? "dark" : "light");
    }

    const char* const kAppearanceComment =
        "# appearance = light or dark. The Appearance menu rewrites this line.";

    void SeedPresetsFile(const wxFileName& path, bool dark = false)
    {
        if (!wxFileName::DirExists(path.GetPath()))
            wxFileName::Mkdir(path.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

        wxTextFile file(path.GetFullPath());
        if (!file.Create())
            return;

        file.AddLine("# PingOrganizer presets - one target per line.");
        file.AddLine("# Blank lines and lines starting with # are ignored.");
        file.AddLine("# Edit freely, then restart the app.");
        file.AddLine("# Edits made in the app's preset boxes are saved here too.");
        file.AddLine("#");
        file.AddLine(kAppearanceComment);
        file.AddLine(AppearanceLine(dark));
        file.AddLine(wxEmptyString);
        for (const wxString& preset : kDefaultPresets)
            file.AddLine(preset);
        file.Write();
    }

    struct PresetsConfig
    {
        std::vector<wxString> targets;
        bool dark = false;          // no appearance line means light
    };

    PresetsConfig LoadPresets()
    {
        PresetsConfig config;

        const wxFileName path = PresetsPath();
        MigrateLegacyPresets(path);
        if (!path.FileExists())
            SeedPresetsFile(path);

        wxTextFile file(path.GetFullPath());
        if (file.Open())
        {
            wxString key, value;
            for (size_t i = 0; i < file.GetLineCount(); ++i)
            {
                wxString line = file[i];
                line.Trim(true).Trim(false);
                if (line.empty() || line.StartsWith("#"))
                    continue;

                if (ParseSetting(line, key, value))
                {
                    if (key == kAppearanceKey)
                        config.dark = (value == "dark");
                    continue;               // unknown settings are ignored
                }

                config.targets.push_back(line);
            }
            file.Close();
        }

        if (config.targets.empty())
            config.targets.assign(std::begin(kDefaultPresets), std::end(kDefaultPresets));

        return config;
    }

    // Records the appearance in presets.txt. The file is re-read from disk here
    // rather than rewritten from what was loaded at startup, and only the one
    // line changes, so edits saved in an editor since then survive. Returns
    // false if the file could not be written.
    bool SaveAppearance(bool dark)
    {
        const wxFileName path = PresetsPath();
        if (!path.FileExists())
        {
            SeedPresetsFile(path, dark);
            return path.FileExists();
        }

        wxTextFile file(path.GetFullPath());
        if (!file.Open())
            return false;

        wxString key, value;
        for (size_t i = 0; i < file.GetLineCount(); ++i)
        {
            wxString line = file[i];
            line.Trim(true).Trim(false);
            if (line.StartsWith("#"))
                continue;
            if (ParseSetting(line, key, value) && key == kAppearanceKey)
            {
                file[i] = AppearanceLine(dark);
                return file.Write();
            }
        }

        // No appearance line yet - a file from before this setting existed.
        // Add one straight after the header comments, where it is easy to find.
        size_t at = 0;
        while (at < file.GetLineCount() && file[at].StartsWith("#"))
            ++at;
        file.InsertLine(AppearanceLine(dark), at);
        file.InsertLine(kAppearanceComment, at);
        if (at > 0)
            file.InsertLine("#", at);     // spacer, only when there is a header above
        return file.Write();
    }

    // True for a line LoadPresets() would read as a target.
    bool IsTargetLine(const wxString& raw)
    {
        wxString line = raw;
        line.Trim(true).Trim(false);
        return !line.empty() && !line.StartsWith("#") && line.Find('=') == wxNOT_FOUND;
    }

    // Writes the preset list from the app into presets.txt. Like SaveAppearance
    // it re-reads the file from disk and changes only what it owns - here, the
    // target lines. Comments, blank lines and settings stay exactly where they
    // are; the new list fills the existing target lines in order, surplus old
    // lines are dropped, and extra new targets go after the last one. So
    // grouping and notes written by hand survive. `dark` is only used if the
    // file has to be recreated. Returns false if the file could not be written.
    bool SavePresetTargets(const std::vector<wxString>& targets, bool dark)
    {
        const wxFileName path = PresetsPath();
        if (!path.FileExists())
            SeedPresetsFile(path, dark);

        wxTextFile file(path.GetFullPath());
        if (!file.Open())
            return false;

        size_t next = 0;                // next target to place
        size_t insertAt = 0;            // just after the last target line
        bool sawTarget = false;
        bool changed = false;

        for (size_t i = 0; i < file.GetLineCount(); )
        {
            if (!IsTargetLine(file[i]))
            {
                ++i;
                continue;
            }

            sawTarget = true;
            if (next < targets.size())
            {
                if (file[i] != targets[next])
                {
                    file[i] = targets[next];
                    changed = true;
                }
                ++next;
                insertAt = ++i;
            }
            else
            {
                file.RemoveLine(i);     // fewer targets than before
                changed = true;
            }
        }

        if (!sawTarget)
            insertAt = file.GetLineCount();
        for (; next < targets.size(); ++next)
        {
            file.InsertLine(targets[next], insertAt++);
            changed = true;
        }

        return changed ? file.Write() : true;
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
    constexpr UINT kWmUahInitMenu = 0x0093;
    constexpr DWORD kUahInitPopupFlags = 0x4000001;   // set when a popup is being created

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

    // -----------------------------------------------------------------------
    // Appearance (Appearance > Dark Mode / Light Mode).
    //
    // wxWidgets 3.3 has a dark mode of its own, but on Windows it can only be
    // chosen before the first window exists (wxApp::SetAppearance returns
    // CannotChange after that), so it cannot back a menu toggle. This does the
    // same work by hand, per window, in both directions:
    //
    //  - Title bar: DwmSetWindowAttribute. Documented; the caption and caption
    //    text colour attributes need Windows 11 and are ignored on Windows 10.
    //  - Controls: SetWindowTheme with the DarkMode_* visual-style classes that
    //    Windows' own dark apps, and wxWidgets, use. The names are undocumented.
    //  - Popup menus: SetPreferredAppMode and FlushMenuThemes, which uxtheme
    //    exports by ordinal only. Undocumented.
    //  - Menu bar: the UAH painting above, drawn from the active palette.
    //
    // Every undocumented piece fails soft: a missing ordinal or an ignored theme
    // name leaves that one element in its light appearance, nothing worse.
    // -----------------------------------------------------------------------

    // The dark palette is the terminal's own Campbell scheme (assets/bridge.js),
    // so the window and the shell read as one surface.
    wxColour DarkSurface()      { return wxColour(0x0C, 0x0C, 0x0C); }
    wxColour DarkText()         { return wxColour(0xCC, 0xCC, 0xCC); }
    wxColour DarkDisabledText() { return wxColour(0x6D, 0x6D, 0x6D); }
    wxColour DarkSecondaryText(){ return wxColour(0x9D, 0x9D, 0x9D); }  // menu accelerators
    wxColour DarkLink()         { return wxColour(0x61, 0xD6, 0xD6); }  // Campbell bright cyan
    wxColour DarkHighlight()    { return wxColour(0x2A, 0x2A, 0x2A); }  // hovered menu item
    wxColour DarkSeparator()    { return wxColour(0x3F, 0x3F, 0x3F); }  // as the divider

    // Run buttons: the fill is the one Windows' own dark button style uses,
    // measured from it, so only its border goes.
    wxColour DarkButton()        { return wxColour(0x33, 0x33, 0x33); }
    wxColour DarkButtonHot()     { return wxColour(0x3F, 0x3F, 0x3F); }
    wxColour DarkButtonPressed() { return wxColour(0x26, 0x26, 0x26); }

    COLORREF ToColorRef(const wxColour& colour)
    {
        return RGB(colour.Red(), colour.Green(), colour.Blue());
    }

    // DWMWINDOWATTRIBUTE values, spelled out so this builds against older SDKs.
    constexpr DWORD kDwmUseImmersiveDarkMode = 20;
    constexpr DWORD kDwmWindowCornerPreference = 33;
    constexpr DWORD kDwmBorderColour = 34;
    constexpr DWORD kDwmCaptionColour = 35;
    constexpr DWORD kDwmCaptionTextColour = 36;
    constexpr COLORREF kDwmColourDefault = 0xFFFFFFFF;

    // uxtheme's PreferredAppMode values.
    constexpr int kAppModeForceDark = 2;
    constexpr int kAppModeForceLight = 3;

    struct UxThemeDarkApi
    {
        bool (WINAPI* allowDarkModeForWindow)(HWND, bool) = nullptr;   // ordinal 133
        int (WINAPI* setPreferredAppMode)(int) = nullptr;              // ordinal 135
        void (WINAPI* flushMenuThemes)() = nullptr;                    // ordinal 136
    };

    const UxThemeDarkApi& DarkApi()
    {
        static const UxThemeDarkApi api = []
        {
            UxThemeDarkApi found;
            HMODULE uxtheme = ::LoadLibraryExW(L"uxtheme.dll", nullptr,
                                               LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (uxtheme)
            {
                found.allowDarkModeForWindow =
                    reinterpret_cast<decltype(found.allowDarkModeForWindow)>(
                        ::GetProcAddress(uxtheme, MAKEINTRESOURCEA(133)));
                found.setPreferredAppMode =
                    reinterpret_cast<decltype(found.setPreferredAppMode)>(
                        ::GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
                found.flushMenuThemes =
                    reinterpret_cast<decltype(found.flushMenuThemes)>(
                        ::GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
            }
            return found;
        }();
        return api;
    }

    void SetTitleBarDark(HWND hwnd, bool dark)
    {
        const BOOL immersive = dark ? TRUE : FALSE;
        ::DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode,
                                &immersive, sizeof(immersive));

        // Immersive dark mode alone gives a dark grey caption; these make it
        // the same black as the rest of the window.
        const COLORREF caption = dark ? ToColorRef(DarkSurface()) : kDwmColourDefault;
        const COLORREF text = dark ? ToColorRef(DarkText()) : kDwmColourDefault;
        ::DwmSetWindowAttribute(hwnd, kDwmCaptionColour, &caption, sizeof(caption));
        ::DwmSetWindowAttribute(hwnd, kDwmCaptionTextColour, &text, sizeof(text));
    }

    // Popup menu background while dark - the strip above the first item and
    // below the last, which no item paints. One brush for the life of the
    // process, as menus keep a reference to it.
    HBRUSH DarkMenuBrush()
    {
        static const HBRUSH brush = ::CreateSolidBrush(ToColorRef(DarkSurface()));
        return brush;
    }

    // Windows 11 draws a popup menu's outline and corners with DWM, and gives
    // an owner-drawn popup square corners and a light outline. This restores
    // the rounded corners Windows' own menus have and makes the outline match
    // the dark items. The class drop shadow is square, so it goes too - the
    // same thing wxWidgets does for its own owner-drawn menus.
    void StyleDarkPopupMenu(HWND popup)
    {
        const int roundSmall = 3;                   // DWMWCP_ROUNDSMALL
        ::DwmSetWindowAttribute(popup, kDwmWindowCornerPreference,
                                &roundSmall, sizeof(roundSmall));

        const DWORD classStyle = static_cast<DWORD>(::GetClassLongPtrW(popup, GCL_STYLE));
        if (classStyle & CS_DROPSHADOW)
            ::SetClassLongPtrW(popup, GCL_STYLE, classStyle & ~CS_DROPSHADOW);

        const COLORREF border = ToColorRef(DarkSeparator());
        ::DwmSetWindowAttribute(popup, kDwmBorderColour, &border, sizeof(border));
    }

    // The font Windows itself uses for menus, at the given window's DPI.
    HFONT CreateMenuFont(HWND hwnd)
    {
        NONCLIENTMETRICSW metrics = {};
        metrics.cbSize = sizeof(metrics);
        if (!::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                          &metrics, 0, ::GetDpiForWindow(hwnd)))
            return nullptr;
        return ::CreateFontIndirectW(&metrics.lfMenuFont);
    }

    // "E&xit\tAlt+F4" -> "E&xit" and "Alt+F4".
    struct MenuLabel
    {
        std::wstring text;
        std::wstring accel;
    };

    MenuLabel SplitMenuLabel(const wxString& label)
    {
        const std::wstring full = label.ToStdWstring();
        const size_t tab = full.find(L'\t');
        if (tab == std::wstring::npos)
            return { full, std::wstring() };
        return { full.substr(0, tab), full.substr(tab + 1) };
    }

    // Switch one native control between its stock visual style and a dark
    // variant. Light mode passes null for both names, which removes the
    // override and restores the stock theme exactly.
    void SetControlTheme(HWND hwnd, bool dark,
                         const wchar_t* darkClass, const wchar_t* darkIdList = nullptr)
    {
        if (!hwnd)
            return;

        if (DarkApi().allowDarkModeForWindow)
            DarkApi().allowDarkModeForWindow(hwnd, dark);

        if (dark)
            ::SetWindowTheme(hwnd, darkClass, darkIdList);
        else
            ::SetWindowTheme(hwnd, nullptr, nullptr);

        ::SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
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

// The ▶ run button - the top bar's and every preset row's. In light mode it is
// a stock wxButton. In dark mode it paints itself: Windows' dark button style
// draws a two-pixel light border that no style bit or colour setting removes,
// so custom draw - the hook comctl32 provides for exactly this, and which also
// covers the hover and press repaints the button does outside WM_PAINT - takes
// over the whole paint. It keeps the dark style's own fill and white glyph and
// drops only the border.
class RunButton : public wxButton
{
public:
    RunButton(wxWindow* parent, wxWindowID id, const wxString& label, const bool& dark)
        : wxButton(parent, id, label, wxDefaultPosition, wxSize(34, -1)),
          m_dark(dark)
    {
    }

    bool MSWOnNotify(int idCtrl, WXLPARAM lParam, WXLPARAM* result) override
    {
        const auto* draw = reinterpret_cast<const NMCUSTOMDRAW*>(lParam);
        if (!m_dark || draw->hdr.code != NM_CUSTOMDRAW)
            return wxButton::MSWOnNotify(idCtrl, lParam, result);

        // Paint over the stock rendering after it is done, rather than skip it:
        // a themed button draws into an off-screen buffer, and returning
        // CDRF_SKIPDEFAULT makes it throw that buffer away - anything drawn at
        // pre-paint never reaches the screen, leaving a blank grey box. At
        // post-paint the drawing lands in the same buffer just before it is
        // copied out, so it shows and never flickers.
        if (draw->dwDrawStage == CDDS_PREPAINT)
        {
            *result = CDRF_NOTIFYPOSTPAINT;
        }
        else
        {
            if (draw->dwDrawStage == CDDS_POSTPAINT)
                PaintDark(*draw);
            *result = CDRF_DODEFAULT;
        }
        return true;
    }

private:
    void PaintDark(const NMCUSTOMDRAW& draw) const
    {
        HDC hdc = draw.hdc;
        const RECT& rc = draw.rc;
        const UINT state = draw.uItemState;

        // What shows behind the rounded corners.
        FillWith(hdc, rc, ToColorRef(DarkSurface()));

        wxColour fill = DarkButton();
        if (state & CDIS_SELECTED)
            fill = DarkButtonPressed();
        else if (state & CDIS_HOT)
            fill = DarkButtonHot();

        const int inset = FromDIP(1);
        const int corner = FromDIP(6);
        HBRUSH brush = ::CreateSolidBrush(ToColorRef(fill));
        HGDIOBJ oldBrush = ::SelectObject(hdc, brush);
        HGDIOBJ oldPen = ::SelectObject(hdc, ::GetStockObject(NULL_PEN));
        // With a null pen RoundRect stops a pixel short on the right and
        // bottom, hence the +1.
        ::RoundRect(hdc, rc.left + inset, rc.top + inset,
                    rc.right - inset + 1, rc.bottom - inset + 1, corner, corner);

        // Keyboard focus only: a quiet grey outline once Tab has been used,
        // never for the mouse, so the button stays findable from the keyboard.
        if ((state & CDIS_FOCUS) && (state & CDIS_SHOWKEYBOARDCUES))
        {
            HPEN pen = ::CreatePen(PS_SOLID, 1, ToColorRef(DarkDisabledText()));
            ::SelectObject(hdc, pen);
            ::SelectObject(hdc, ::GetStockObject(NULL_BRUSH));
            ::RoundRect(hdc, rc.left + inset, rc.top + inset,
                        rc.right - inset, rc.bottom - inset, corner, corner);
            ::SelectObject(hdc, ::GetStockObject(NULL_PEN));
            ::DeleteObject(pen);
        }

        ::SelectObject(hdc, oldPen);
        ::SelectObject(hdc, oldBrush);
        ::DeleteObject(brush);

        RECT textRect = rc;
        HGDIOBJ oldFont = ::SelectObject(hdc, static_cast<HFONT>(GetFont().GetHFONT()));
        ::SetBkMode(hdc, TRANSPARENT);
        ::SetTextColor(hdc, RGB(0xFF, 0xFF, 0xFF));
        const std::wstring label = GetLabel().ToStdWstring();
        ::DrawTextW(hdc, label.c_str(), -1, &textRect,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        ::SelectObject(hdc, oldFont);
    }

    const bool& m_dark;     // the frame's, which outlives every button
};

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
    COLORREF MenuBarBack() const;

    // Popup menus (File, Appearance, Help) are owner-drawn while dark: Windows'
    // dark menu style is grey, not black, and has no colour settings.
    void SetMenusOwnerDrawn(bool ownerDrawn);
    wxMenuItem* DrawnMenuItem(ULONG_PTR itemData) const;
    bool MeasureMenuItem(WXLPARAM lParam);
    bool DrawMenuItem(WXLPARAM lParam);

    // Re-colours the whole frame for the current m_dark, in either direction.
    void ApplyAppearance();
    // Re-colours one window and everything under it. Also used for the About
    // dialog and for preset rows created after the switch.
    void ThemeTree(wxWindow* window);

    void OnRun(wxCommandEvent& event);
    void OnOpenPresets(wxCommandEvent& event);
    void OnToggleDarkMode(wxCommandEvent& event);
    void OnClose(wxCloseEvent& event);

    // Writes the preset boxes to presets.txt if any have been edited since the
    // last save. Called when a box loses focus, before a preset is run, and on
    // close - so an edit is on disk by the time anything else happens.
    void SavePresetEdits();
    void OnExit(wxCommandEvent& event);
    void OnAbout(wxCommandEvent& event);
    void OnPresetAreaSize(wxSizeEvent& event);

    // Every entry point funnels here: the top bar, each preset row, and Enter in
    // any of their textboxes. `source` is the box to put focus back into when the
    // target is blank.
    void RunFor(const wxString& raw, wxTextCtrl* source);

    void BuildMenuBar();
    wxWindow* BuildDivider();
    wxWindow* BuildPresets(wxWindow* parent, const std::vector<wxString>& targets);
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

    bool m_dark = false;
    wxMenuItem* m_appearanceItem = nullptr;   // reads "Dark Mode" or "Light Mode"

    // The items currently owner-drawn by us. WM_MEASUREITEM and WM_DRAWITEM
    // carry only an opaque pointer, so it is looked up here before use; any
    // item not in this set is handed on to wxWidgets untouched.
    std::set<wxMenuItem*> m_drawnMenuItems;

    std::vector<wxTextCtrl*> m_presetBoxes;   // in row order, spare rows included
    bool m_presetsDirty = false;              // a box changed since the last save
    bool m_closing = false;                   // no saves once teardown begins
};

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "Test-Connection", wxDefaultPosition, wxSize(1180, 700))
{
    SetMinSize(wxSize(780, 320));

    // Read once, up front: the targets build the presets column, and the saved
    // appearance is applied before the window is ever shown.
    const PresetsConfig config = LoadPresets();

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
    grid->Add(BuildDivider(), 0, wxEXPAND);
    grid->Add(BuildPresets(this, config.targets), 0, wxEXPAND);

    SetSizer(grid);

    CreateStatusBar();
    SetStatusText("Starting PowerShell 7...");

    Bind(wxEVT_BUTTON, &MainFrame::OnRun, this, wxID_OK);
    m_target->Bind(wxEVT_TEXT_ENTER, &MainFrame::OnRun, this);
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);

    run->SetDefault();
    m_target->SetFocus();

    // Last, so every control - status bar included - exists to be themed. The
    // frame is not shown yet, so a dark start never flashes light first.
    if (config.dark)
    {
        m_dark = true;
        m_appearanceItem->SetItemLabel("&Light Mode");
        ApplyAppearance();
    }
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

        // Popup menu items, while they are ours. Answered before wxWidgets
        // sees them, which would otherwise take the item data for one of its
        // own owner-drawn items.
        case WM_MEASUREITEM:
            if (MeasureMenuItem(lParam))
                return TRUE;
            break;

        case WM_DRAWITEM:
            if (DrawMenuItem(lParam))
                return TRUE;
            break;

        // A popup menu being created; for a popup, its window is behind the DC.
        // Styling it here, before it is first shown, means it opens dark-edged
        // rather than turning dark a moment later.
        case kWmUahInitMenu:
            if (m_dark)
            {
                const auto* menu = reinterpret_cast<const UahMenu*>(lParam);
                if (menu && menu->hdc && (menu->dwFlags & kUahInitPopupFlags))
                    if (HWND popup = ::WindowFromDC(menu->hdc))
                        StyleDarkPopupMenu(popup);
            }
            break;                  // still goes on to the default handling

        // An open popup menu going idle; lParam is its window. This recurs every
        // time the pointer moves to another item. wxWidgets' only use of it is
        // to give owner-drawn popups rounded corners and the *light* theme's
        // outline, so while dark it must not see it at all: letting it run and
        // correcting afterwards flashed the light outline between items.
        case WM_ENTERIDLE:
            if (m_dark && wParam == MSGF_MENU && lParam)
            {
                StyleDarkPopupMenu(reinterpret_cast<HWND>(lParam));
                return 0;
            }
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

    FillWith(menu->hdc, bar, MenuBarBack());
    return true;
}

// The bar is painted in the same colour as the frame beneath it, whichever
// appearance is active - which is the whole point of painting it ourselves.
COLORREF MainFrame::MenuBarBack() const
{
    return ToColorRef(m_dark ? DarkSurface()
                             : wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));
}

// Flips every item of every popup menu between owner-drawn (dark) and stock
// (light). Only the item's type bits change - its text, id and state are left
// alone - so turning owner-draw off hands back exactly the item wxWidgets made.
// Must run after any SetItemLabel(), which may reset an item's type.
void MainFrame::SetMenusOwnerDrawn(bool ownerDrawn)
{
    wxMenuBar* bar = GetMenuBar();
    if (!bar)
        return;

    m_drawnMenuItems.clear();

    for (size_t m = 0; m < bar->GetMenuCount(); ++m)
    {
        wxMenu* menu = bar->GetMenu(m);
        HMENU hmenu = static_cast<HMENU>(menu->GetHMenu());

        MENUINFO info = {};
        info.cbSize = sizeof(info);
        info.fMask = MIM_BACKGROUND;
        info.hbrBack = ownerDrawn ? DarkMenuBrush() : nullptr;   // null: theme's own
        ::SetMenuInfo(hmenu, &info);

        UINT pos = 0;
        for (auto node = menu->GetMenuItems().GetFirst(); node; node = node->GetNext(), ++pos)
        {
            wxMenuItem* item = node->GetData();

            MENUITEMINFOW mii = {};
            mii.cbSize = sizeof(mii);
            mii.fMask = MIIM_FTYPE;
            if (!::GetMenuItemInfoW(hmenu, pos, TRUE, &mii))
                continue;

            if (ownerDrawn)
                mii.fType |= MFT_OWNERDRAW;
            else
                mii.fType &= ~MFT_OWNERDRAW;

            // wxWidgets already stores the wxMenuItem here for text items, but
            // not for separators; store it for all, so the draw handlers can
            // tell what they are drawing.
            mii.fMask |= MIIM_DATA;
            mii.dwItemData = reinterpret_cast<ULONG_PTR>(item);
            ::SetMenuItemInfoW(hmenu, pos, TRUE, &mii);

            if (ownerDrawn)
                m_drawnMenuItems.insert(item);
        }
    }
}

wxMenuItem* MainFrame::DrawnMenuItem(ULONG_PTR itemData) const
{
    auto* item = reinterpret_cast<wxMenuItem*>(itemData);
    return m_drawnMenuItems.count(item) ? item : nullptr;
}

bool MainFrame::MeasureMenuItem(WXLPARAM lParam)
{
    auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
    if (measure->CtlType != ODT_MENU)
        return false;
    wxMenuItem* item = DrawnMenuItem(measure->itemData);
    if (!item)
        return false;

    if (item->IsSeparator())
    {
        measure->itemWidth = 0;
        measure->itemHeight = FromDIP(9);
        return true;
    }

    HWND hwnd = static_cast<HWND>(GetHandle());
    HDC hdc = ::GetDC(hwnd);
    HFONT font = CreateMenuFont(hwnd);
    HGDIOBJ oldFont = font ? ::SelectObject(hdc, font) : nullptr;

    const MenuLabel label = SplitMenuLabel(item->GetItemLabel());
    RECT textSize = {};
    ::DrawTextW(hdc, label.text.c_str(), -1, &textSize, DT_CALCRECT | DT_SINGLELINE);
    RECT accelSize = {};
    if (!label.accel.empty())
        ::DrawTextW(hdc, label.accel.c_str(), -1, &accelSize,
                    DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);

    if (oldFont)
        ::SelectObject(hdc, oldFont);
    if (font)
        ::DeleteObject(font);
    ::ReleaseDC(hwnd, hdc);

    const int padX = FromDIP(14);
    int width = padX + (textSize.right - textSize.left) + padX;
    if (!label.accel.empty())
        width += FromDIP(32) + (accelSize.right - accelSize.left);

    measure->itemWidth = static_cast<UINT>(width);
    measure->itemHeight = static_cast<UINT>((textSize.bottom - textSize.top) + FromDIP(12));
    return true;
}

bool MainFrame::DrawMenuItem(WXLPARAM lParam)
{
    auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
    if (draw->CtlType != ODT_MENU)
        return false;
    wxMenuItem* item = DrawnMenuItem(draw->itemData);
    if (!item)
        return false;

    HDC hdc = draw->hDC;
    const RECT rc = draw->rcItem;
    FillWith(hdc, rc, ToColorRef(DarkSurface()));

    if (item->IsSeparator())
    {
        RECT line = rc;
        line.left += FromDIP(10);
        line.right -= FromDIP(10);
        line.top = (rc.top + rc.bottom) / 2;
        line.bottom = line.top + 1;
        FillWith(hdc, line, ToColorRef(DarkSeparator()));
        return true;
    }

    const bool disabled = (draw->itemState & (ODS_GRAYED | ODS_DISABLED)) != 0;
    if ((draw->itemState & ODS_SELECTED) && !disabled)
    {
        RECT hot = rc;
        ::InflateRect(&hot, -FromDIP(4), -FromDIP(1));
        FillWith(hdc, hot, ToColorRef(DarkHighlight()));
    }

    HFONT font = CreateMenuFont(static_cast<HWND>(GetHandle()));
    HGDIOBJ oldFont = font ? ::SelectObject(hdc, font) : nullptr;
    ::SetBkMode(hdc, TRANSPARENT);

    const MenuLabel label = SplitMenuLabel(item->GetItemLabel());
    RECT text = rc;
    text.left += FromDIP(14);
    text.right -= FromDIP(14);

    UINT flags = DT_SINGLELINE | DT_VCENTER;
    if (draw->itemState & ODS_NOACCEL)
        flags |= DT_HIDEPREFIX;             // Alt not held: no mnemonic underline

    ::SetTextColor(hdc, ToColorRef(disabled ? DarkDisabledText() : DarkText()));
    ::DrawTextW(hdc, label.text.c_str(), -1, &text, flags | DT_LEFT);

    if (!label.accel.empty())
    {
        ::SetTextColor(hdc, ToColorRef(disabled ? DarkDisabledText() : DarkSecondaryText()));
        ::DrawTextW(hdc, label.accel.c_str(), -1, &text,
                    DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    }

    if (oldFont)
        ::SelectObject(hdc, oldFont);
    if (font)
        ::DeleteObject(font);
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

    const COLORREF face = MenuBarBack();
    COLORREF back = face;
    COLORREF fore = m_dark ? ToColorRef(DarkText()) : ::GetSysColor(COLOR_MENUTEXT);

    // Hover and open states step away from the bar colour: darker on a light
    // bar, lighter on a dark one.
    const int step = m_dark ? 1 : -1;
    if (item->dis.itemState & ODS_HOTLIGHT)
        back = Shade(face, step * (m_dark ? 30 : 16));
    if (item->dis.itemState & ODS_SELECTED)      // its popup is open
        back = Shade(face, step * (m_dark ? 48 : 30));
    if (item->dis.itemState & (ODS_GRAYED | ODS_DISABLED))
        fore = m_dark ? ToColorRef(DarkDisabledText()) : ::GetSysColor(COLOR_GRAYTEXT);

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
    FillWith(hdc, line, MenuBarBack());
    ::ReleaseDC(hwnd, hdc);
}

// U+25B6 BLACK RIGHT-POINTING TRIANGLE, built from the code point so the label
// does not depend on how this file happens to be encoded. A column of identical
// glyphs reads as texture and lets the eye go straight to the addresses; a
// column of identical words has to be re-read every pass.
wxButton* MainFrame::MakeRunButton(wxWindow* parent, wxWindowID id)
{
    const wxString runGlyph(wxUniChar(0x25B6));

    wxButton* button = new RunButton(parent, id, runGlyph, m_dark);

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

    // One item whose label names the mode it switches *to*.
    wxMenu* appearanceMenu = new wxMenu;
    m_appearanceItem = appearanceMenu->Append(ID_ToggleDarkMode, "&Dark Mode");

    wxMenu* helpMenu = new wxMenu;
    helpMenu->Append(wxID_ABOUT, "&About\tF1");

    wxMenuBar* menus = new wxMenuBar;
    menus->Append(fileMenu, "&File");
    menus->Append(appearanceMenu, "&Appearance");
    menus->Append(helpMenu, "&Help");
    SetMenuBar(menus);

    // The bar itself is painted in MSWWindowProc - see the long note on the
    // UAH messages near the top of this file.

    Bind(wxEVT_MENU, &MainFrame::OnOpenPresets, this, ID_OpenPresets);
    Bind(wxEVT_MENU, &MainFrame::OnToggleDarkMode, this, ID_ToggleDarkMode);
    Bind(wxEVT_MENU, &MainFrame::OnExit, this, wxID_EXIT);
    Bind(wxEVT_MENU, &MainFrame::OnAbout, this, wxID_ABOUT);
}

// The line between the terminal and the presets column. This was a
// wxStaticLine, but Windows draws that as an etched shadow/highlight pair in
// fixed system colours - on a black window, a bright white stripe. So it is
// painted here instead: in light mode with the very same two system colours,
// so it looks exactly as before, and in dark mode as one quiet grey line.
wxWindow* MainFrame::BuildDivider()
{
    wxWindow* divider = new wxWindow(this, wxID_ANY, wxDefaultPosition, wxSize(2, -1));
    divider->SetMinSize(wxSize(2, -1));
    divider->SetBackgroundStyle(wxBG_STYLE_PAINT);

    divider->Bind(wxEVT_PAINT, [this, divider](wxPaintEvent&)
    {
        wxPaintDC dc(divider);
        const int height = divider->GetClientSize().y;

        const wxColour shadow = m_dark
            ? wxColour(0x3F, 0x3F, 0x3F)        // the terminal's scrollbar thumb
            : wxSystemSettings::GetColour(wxSYS_COLOUR_BTNSHADOW);
        const wxColour highlight = m_dark
            ? DarkSurface()
            : wxSystemSettings::GetColour(wxSYS_COLOUR_BTNHIGHLIGHT);

        dc.SetPen(wxPen(shadow));
        dc.DrawLine(0, 0, 0, height);
        dc.SetPen(wxPen(highlight));
        dc.DrawLine(1, 0, 1, height);
    });

    return divider;
}

wxWindow* MainFrame::BuildPresets(wxWindow* parent, const std::vector<wxString>& targets)
{
    m_presetArea = new wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition,
                                        wxDefaultSize, wxVSCROLL);
    m_presetArea->SetScrollRate(0, 8);
    m_presetArea->SetMinSize(wxSize(250, -1));
    m_presetArea->SetBackgroundColour(
        wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));

    wxStaticText* heading = new wxStaticText(m_presetArea, wxID_ANY, "Presets");
    heading->SetToolTip(PresetsPath().GetFullPath() +
                        "\n\nEdits in the boxes below are saved there automatically."
                        "\nFile > Presets opens the file; restart the app to apply"
                        "\nchanges made to it directly.");

    // Two columns - textbox, button - one row per preset.
    m_presetGrid = new wxFlexGridSizer(2, 6, 8);
    m_presetGrid->AddGrowableCol(0, 1);

    for (const wxString& preset : targets)
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

    auto run = [this, box](wxCommandEvent&)
    {
        SavePresetEdits();          // Enter keeps focus in the box, so save here too
        RunFor(box->GetValue(), box);
    };
    go->Bind(wxEVT_BUTTON, run);
    box->Bind(wxEVT_TEXT_ENTER, run);

    // Edits are remembered as they happen and written when the box is left.
    box->Bind(wxEVT_TEXT, [this](wxCommandEvent& event)
    {
        m_presetsDirty = true;
        event.Skip();
    });
    box->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event)
    {
        SavePresetEdits();
        event.Skip();
    });
    m_presetBoxes.push_back(box);

    // Rows are added on resize, so one can be born after the switch to dark.
    // In light mode there is nothing to do: new controls start out stock.
    if (m_dark)
    {
        ThemeTree(box);
        ThemeTree(go);
    }

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
        SeedPresetsFile(path, m_dark);

    if (wxLaunchDefaultApplication(path.GetFullPath()))
        SetStatusText(path.GetFullPath() + "  (restart to apply changes)");
    else
        SetStatusText("Could not open " + path.GetFullPath());
}

void MainFrame::OnExit(wxCommandEvent& WXUNUSED(event))
{
    Close(true);
}

void MainFrame::OnClose(wxCloseEvent& event)
{
    // The box being typed in may still have focus, so no kill-focus save has
    // happened for it. After this, children are torn down and focus events
    // could reach boxes whose neighbours are already gone - so stop saving.
    SavePresetEdits();
    m_closing = true;
    event.Skip();                   // carry on with the normal close
}

void MainFrame::SavePresetEdits()
{
    if (!m_presetsDirty || m_closing || IsBeingDeleted())
        return;

    std::vector<wxString> targets;
    wxString leftOut;
    for (wxTextCtrl* box : m_presetBoxes)
    {
        wxString value = box->GetValue();
        value.Trim(true).Trim(false);
        if (value.empty())
            continue;               // spare or cleared row

        // Would not read back as a target: '=' makes a settings line - typing
        // "appearance = dark" into a box must not flip the setting - and a
        // leading '#' a comment. It stays in the box, just not in the file.
        if (!IsTargetLine(value))
        {
            leftOut = value;
            continue;
        }
        targets.push_back(value);
    }

    const wxString path = PresetsPath().GetFullPath();
    if (!SavePresetTargets(targets, m_dark))
    {
        // Still dirty, so the next chance to save tries again.
        SetStatusText("Could not save presets to " + path);
        return;
    }

    m_presetsDirty = false;
    if (leftOut.empty())
        SetStatusText("Presets saved to " + path);
    else
        SetStatusText("Presets saved, except \"" + leftOut +
                      "\" - a preset cannot contain '=' or start with '#'");
}

void MainFrame::OnToggleDarkMode(wxCommandEvent& WXUNUSED(event))
{
    m_dark = !m_dark;
    m_appearanceItem->SetItemLabel(m_dark ? "&Light Mode" : "&Dark Mode");
    ApplyAppearance();

    // The switch itself has already happened; failing to record it only means
    // the next launch starts in the previous appearance, so say so and move on.
    if (!SaveAppearance(m_dark))
        SetStatusText("Could not save the appearance to " + PresetsPath().GetFullPath());
}

void MainFrame::ApplyAppearance()
{
    HWND hwnd = static_cast<HWND>(GetHandle());

    // Popup menus follow a process-wide mode rather than a per-window one.
    // Only touched when dark is in play - a dark start or a toggle - so a
    // session that stays light keeps the stock Default mode.
    if (DarkApi().setPreferredAppMode)
    {
        DarkApi().setPreferredAppMode(m_dark ? kAppModeForceDark : kAppModeForceLight);
        if (DarkApi().flushMenuThemes)
            DarkApi().flushMenuThemes();
    }
    if (DarkApi().allowDarkModeForWindow)
        DarkApi().allowDarkModeForWindow(hwnd, m_dark);

    SetTitleBarDark(hwnd, m_dark);
    SetMenusOwnerDrawn(m_dark);
    ThemeTree(this);

    // The menu bar and title bar are non-client area, which Refresh() never
    // reaches; ask for both explicitly.
    ::DrawMenuBar(hwnd);
    ::RedrawWindow(hwnd, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void MainFrame::ThemeTree(wxWindow* window)
{
    // Already black in both modes, and its WebView2 child is not a wx window.
    if (window == m_terminal)
        return;

    const bool dark = m_dark;
    HWND hwnd = static_cast<HWND>(window->GetHandle());

    // Light mode restores exactly what construction set up: the frame and the
    // presets column carry an explicit BTNFACE, every other window is stock.
    const wxColour lightBack = (window == this || window == m_presetArea)
        ? wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE)
        : wxNullColour;
    const wxColour back = dark ? DarkSurface() : lightBack;
    const wxColour fore = dark ? DarkText() : wxNullColour;

    if (wxDynamicCast(window, wxTextCtrl))
    {
        window->SetBackgroundColour(dark ? DarkSurface() : wxNullColour);
        window->SetForegroundColour(fore);
        SetControlTheme(hwnd, dark, L"DarkMode_CFD");
    }
    else if (wxDynamicCast(window, wxChoice))
    {
        window->SetBackgroundColour(dark ? DarkSurface() : wxNullColour);
        window->SetForegroundColour(fore);
        SetControlTheme(hwnd, dark, L"DarkMode_CFD");

        // The dropped-down list is a separate native window with its own
        // scrollbar; theme it too or it opens white.
        COMBOBOXINFO info = {};
        info.cbSize = sizeof(info);
        if (::GetComboBoxInfo(hwnd, &info))
            SetControlTheme(info.hwndList, dark, L"DarkMode_Explorer");
    }
    else if (wxDynamicCast(window, wxButton))
    {
        // Themed push buttons ignore wx colours; the visual style does it all.
        SetControlTheme(hwnd, dark, L"DarkMode_Explorer");
    }
    else if (wxDynamicCast(window, wxStatusBar))
    {
        // Same combination wxWidgets' own dark mode uses: an id list, no class.
        SetControlTheme(hwnd, dark, nullptr, L"ExplorerStatusBar");
    }
    else if (auto* link = wxDynamicCast(window, wxHyperlinkCtrl))
    {
        // Only ever reached dark: the About dialog is rebuilt on each opening
        // and themed only when dark, so light keeps the stock link colours.
        // Stock link blue is too dim on black.
        if (dark)
        {
            link->SetBackgroundColour(back);
            link->SetNormalColour(DarkLink());
            link->SetVisitedColour(DarkLink());
            link->SetHoverColour(DarkLink());
        }
    }
    else
    {
        // Frames, panels, labels, dividers.
        window->SetBackgroundColour(back);
        window->SetForegroundColour(fore);

        // The presets column's scrollbar belongs to its own window.
        if (window == m_presetArea)
            SetControlTheme(hwnd, dark, L"DarkMode_Explorer");
    }

    for (wxWindow* child : window->GetChildren())
        ThemeTree(child);

    window->Refresh();
}

void MainFrame::OnAbout(wxCommandEvent& WXUNUSED(event))
{
    wxDialog dlg(this, wxID_ANY, "About PingOrganizer");

    wxStaticText* name = new wxStaticText(&dlg, wxID_ANY, "PingOrganizer");
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

    // Built fresh each time, so it only needs theming when dark.
    if (m_dark)
    {
        SetTitleBarDark(static_cast<HWND>(dlg.GetHandle()), true);
        ThemeTree(&dlg);
    }

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

class PingOrganizerApp : public wxApp
{
public:
    bool OnInit() override
    {
        if (!wxApp::OnInit())
            return false;

        SetAppName("PingOrganizer");
        (new MainFrame())->Show();
        return true;
    }
};

wxIMPLEMENT_APP(PingOrganizerApp);
