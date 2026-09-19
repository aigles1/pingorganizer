# pingorganizer

![pingorganizer pinging 1.1.1.1 four times, with the presets column on the right](docs/screenshot.png)

A C++ wxWidgets window with a target textbox, a ▶ run button and a 1–10 Count
dropdown, above a **real** embedded PowerShell 7 terminal, with a column of
editable preset targets down the right-hand side, loaded from a text file you
control. Pressing any ▶ types

```
Test-Connection -TargetName '<target>' -Count <n>
```

into that shell, exactly as if you had typed it yourself.

The terminal is not a log pane. It is a full interactive `pwsh.exe` with
PSReadLine, tab completion, syntax colouring, history, `Ctrl+C`, and resize —
the same behaviour you get inside Windows Terminal.

**Tested on Windows 11.**

## Download

Grab `pingorganizer-v0.1-win-x64.zip` from the
[v0.1 release](https://github.com/aigles1/pingorganizer/releases/tag/v0.1),
unzip it anywhere, and run `PSPingGui.exe`. Keep the `assets` folder next to
the exe — the terminal is loaded from it.

Nothing else to install on Windows 11 beyond PowerShell 7: the C++ runtime is
linked into the exe, and the WebView2 Runtime ships with Windows 11.

## How it works

Three pieces:

| Piece | Job |
|---|---|
| **ConPTY** (`src/ConPty.cpp`) | Creates a Windows pseudo console with `CreatePseudoConsole`, spawns `pwsh.exe` attached to it, and pumps the two VT byte pipes. PowerShell cannot tell this apart from Windows Terminal. |
| **xterm.js** (`assets/`) | Parses the VT stream and renders it — glyphs, 24-bit colour, selection, scrollback. It is the same emulator VS Code uses. |
| **WebView2** (`src/TerminalPanel.cpp`) | Hosts xterm.js as a child HWND filling a `wxPanel`, and carries bytes both ways over `postMessage`. |

wxWidgets has no terminal control, and writing a VT emulator by hand is weeks of
work, so the renderer is borrowed rather than rebuilt.

```
 wxTextCtrl / wxButton / wxChoice        (plain wxWidgets)
 ─────────────────────────────────────
 TerminalPanel (wxPanel)
   └── WebView2 HWND
         └── xterm.js  ──postMessage──┐
                                      │
                              ConPty  │  base64'd VT bytes
                                │     │
                                └── pwsh.exe on a pseudo console
```

### Message protocol

Plain strings across the WebView2 bridge:

| Direction | Message | Meaning |
|---|---|---|
| page → host | `ready:<cols>,<rows>` | terminal laid out; start the shell |
| page → host | `i:<base64>` | keystrokes / paste |
| page → host | `s:<cols>,<rows>` | resized → `ResizePseudoConsole` |
| host → page | `o:<base64>` | raw pty output |
| host → page | `focus` | focus the terminal |
| host → page | `x:<text>` | host status line, drawn dim |

## Building

Needs Visual Studio 2026 (or 2022) with the C++ workload, CMake ≥ 3.24, and an
internet connection for the first configure.

```bash
pwsh -File build.ps1 -Run
```

The first run downloads wxWidgets 3.3.2, the WebView2 SDK and xterm.js, then
builds wxWidgets statically — five to ten minutes. After that, rebuilds take
seconds. `-Clean` starts over; `-Config Debug` builds a debug binary.

Output lands in `build/Release/PSPingGui.exe` with an `assets/` folder beside
it. Both are needed. The WebView2 loader and the MSVC runtime are both linked
statically, so there is no extra DLL to copy and no Visual C++ Redistributable
to install.

### Runtime requirements

- Tested on Windows 11. Should work on Windows 10 1809 or newer (ConPTY), but
  that has not been tested.
- The Evergreen WebView2 Runtime — preinstalled on Windows 11
- PowerShell 7 (`pwsh.exe`); found via `PATH`, then the usual install locations

## Notes

**Presets.** The right-hand column is read from a plain text file:

```
%APPDATA%\PSPingGui\presets.txt
```

One target per line; blank lines and `#` comments ignored. The file is created
with a default list on first run, so there is something to edit rather than a
blank page, and the app falls back to that built-in list (`kDefaultPresets` in
`src/main.cpp`) if the file is missing, unreadable or empty. Changes take effect
on restart. Hovering the **Presets** heading shows the full path.

It lives under `%APPDATA%` rather than beside the exe because `build.ps1 -Clean`
deletes `build/` wholesale, which would take user config with it.

Below the configured entries the column keeps generating blank rows until it
reaches the bottom of the window, so spare slots are always to hand. Rows are
only ever added, never removed, so anything typed into a spare row survives a
resize; the fill loop looks one row ahead to stop flush with the bottom instead
of raising a scrollbar. `kMaxPresetRows` caps it on very tall displays.

Every run button — the top bar's and each preset row's — is labelled with
U+25B6 rather than a word, and they are all built by `MainFrame::MakeRunButton`.
Identical words have to be re-read on every pass and compete with the addresses,
which are the only part that varies; identical glyphs read as texture and the eye
skips them. The label comes from the code point, so it does not depend on this
file's encoding. Because a glyph carries no name for assistive tech, each button
gets a tooltip and a window name saying "Ping this target" — note that a full fix
would need a `wxAccessible` subclass, which is not here.

Those buttons also deliberately do *not* use `wxID_OK`, because the frame-level
handler claims that id for the top bar. Every entry point (top bar, preset
button, Enter in any box) funnels into `MainFrame::RunFor`, so the Count dropdown
applies to presets too.

**Menus.** File → Presets opens `presets.txt` in whatever application is
registered for `.txt` (recreating the file first if it has been deleted), File →
Exit closes the app, and Help → About shows a clickable project link.
`kProjectUrl` at the top of `src/main.cpp` points at this repository.

**Window colour, and the menu bar.** The frame and presets panel use
`COLOR_BTNFACE` (`#F0F0F0`). Windows 11 would paint the menu bar above them
`COLOR_WINDOW` white, and there is no supported way to change that: wxWidgets'
`wxMenuBar::SetBackgroundColour` is a no-op on MSW, and the Win32
`SetMenuInfo`/`MIM_BACKGROUND` brush reaches popup menus but not the bar. Both
were tried and measured.

So `MainFrame::MSWWindowProc` answers the undocumented **UAH** messages instead
— `WM_UAHDRAWMENU` (`0x0091`) paints the bar, `WM_UAHDRAWMENUITEM` (`0x0092`)
paints each item, and `WM_NCACTIVATE`/`WM_NCPAINT` cover the hairline Windows
draws underneath. This is the technique dark-mode applications use. The
structures the OS passes are absent from the SDK and are declared in
`src/main.cpp` to match.

Measured result: bar `#F0F0F0`, hovered item `#E0E0E0`, open item `#D2D2D2`.
Popup menus are left alone and stay themed.

Being undocumented, this is the one part of the app Microsoft could break. The
failure mode is benign: if a future Windows stops sending the messages the
handlers never run, and the bar goes back to white while everything else keeps
working. Deleting the three handlers and their structures reverts it.

**Top row alignment.** The target box and the terminal are cells in the same
column of one `wxFlexGridSizer`, which is what makes them exactly the same width
— measured at 912 px for both. The box is added with no left or right border,
since either would make it narrower than the terminal below. Column 1 of that
grid exists only to carry the vertical divider, and that is what gives the top
row its matching gap. The old "Target" label was dropped to buy the alignment;
the box's hint text carries the same information.

**Input safety.** The target string is embedded in a PowerShell single-quoted
string, where the only escape is `''` for a literal quote. `QuoteForPowerShell`
doubles those and drops control characters, so a pasted newline cannot submit a
second command.

**Shutdown ordering.** `ClosePseudoConsole` blocks until the output pipe has
drained, so the reader thread has to still be running when it is called —
joining first deadlocks. `ConPty::Shutdown` closes stdin, waits for `pwsh` to
leave, then closes the pseudo console, and only then joins. For the same reason
pty output reaches the GUI thread through `CallAfter`, which is asynchronous.

**Browser accelerator keys are disabled** (`ICoreWebView2Settings3`). Without
that, WebView2 eats `Ctrl+F`, `Ctrl+P` and `F5` before the shell sees them.

**Assets are served over a virtual https host**, not `file://`, which would
otherwise trip CORS and the page's CSP.

## Layout

```
CMakeLists.txt          fetches deps, builds the exe
build.ps1               one-command build
assets/terminal.html    xterm.js page
assets/bridge.js        renderer side of the protocol
src/main.cpp            window, menus, controls, presets column, command construction
src/TerminalPanel.*     WebView2 hosting + host side of the protocol
src/ConPty.*            pseudo console and child process
src/Base64.h            byte-safe transport for the bridge
```
