// Bridge between xterm.js (renderer) and the C++ host (ConPTY owner).
//
// Wire protocol, all plain strings:
//   page -> host   "ready:<cols>,<rows>"   terminal sized and ready, start the shell
//                  "i:<base64 utf8>"       user input (keystrokes / paste)
//                  "s:<cols>,<rows>"       terminal was resized
//   host -> page   "o:<base64 utf8>"       raw bytes from the pty
//                  "focus"                 give the terminal keyboard focus
//                  "x:<text>"              host status line, drawn dim
(function () {
  'use strict';

  // Windows Terminal's default "Campbell" scheme.
  const CAMPBELL = {
    background: '#0C0C0C', foreground: '#CCCCCC',
    cursor: '#CCCCCC', cursorAccent: '#0C0C0C',
    selectionBackground: '#3A96DD', selectionForeground: '#FFFFFF',
    black: '#0C0C0C', red: '#C50F1F', green: '#13A10E', yellow: '#C19C00',
    blue: '#0037DA', magenta: '#881798', cyan: '#3A96DD', white: '#CCCCCC',
    brightBlack: '#767676', brightRed: '#E74856', brightGreen: '#16C60C',
    brightYellow: '#F9F1A5', brightBlue: '#3B78FF', brightMagenta: '#B4009E',
    brightCyan: '#61D6D6', brightWhite: '#F2F2F2'
  };

  const term = new Terminal({
    fontFamily: '"Cascadia Mono", "Cascadia Code", Consolas, "Courier New", monospace',
    fontSize: 14,
    lineHeight: 1.0,
    cursorBlink: true,
    cursorStyle: 'bar',
    scrollback: 10000,
    allowProposedApi: true,
    windowsPty: { backend: 'conpty' },
    theme: CAMPBELL
  });

  const fitAddon = new FitAddon.FitAddon();
  term.loadAddon(fitAddon);

  const host = window.chrome && window.chrome.webview;
  const el = document.getElementById('term');
  term.open(el);

  function post(s) { if (host) host.postMessage(s); }

  const enc = new TextEncoder();

  function toB64(bytes) {
    let s = '';
    const CHUNK = 0x8000;             // avoid blowing the argument limit on apply()
    for (let i = 0; i < bytes.length; i += CHUNK) {
      s += String.fromCharCode.apply(null, bytes.subarray(i, i + CHUNK));
    }
    return btoa(s);
  }

  function fromB64(b64) {
    const bin = atob(b64);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }

  // ---- keyboard / paste -> pty -------------------------------------------
  term.onData(d => post('i:' + toB64(enc.encode(d))));
  term.onBinary(d => {
    const b = new Uint8Array(d.length);
    for (let i = 0; i < d.length; i++) b[i] = d.charCodeAt(i) & 0xff;
    post('i:' + toB64(b));
  });

  // ---- size ---------------------------------------------------------------
  let cols = 0, rows = 0;
  function announceSize() {
    if (term.cols !== cols || term.rows !== rows) {
      cols = term.cols; rows = term.rows;
      post('s:' + cols + ',' + rows);
    }
  }
  term.onResize(announceSize);

  let fitPending = 0;
  function fitSoon() {
    cancelAnimationFrame(fitPending);
    fitPending = requestAnimationFrame(() => { try { fitAddon.fit(); } catch (e) { /* pre-layout */ } });
  }
  new ResizeObserver(fitSoon).observe(el);
  window.addEventListener('resize', fitSoon);

  // ---- clipboard, matching Windows Terminal's defaults ---------------------
  // Ctrl+V is deliberately left alone: xterm.js's hidden textarea gets a real
  // paste event, which needs no clipboard-read permission.
  term.attachCustomKeyEventHandler(e => {
    if (e.type !== 'keydown' || !e.ctrlKey || e.altKey) return true;
    const k = e.key.toLowerCase();
    if (k === 'c' && (e.shiftKey || term.hasSelection())) {
      const sel = term.getSelection();
      if (sel) {
        navigator.clipboard.writeText(sel).catch(() => {});
        term.clearSelection();
        return false;                 // swallow: copy, don't send ^C
      }
    }
    if (k === 'v' && e.shiftKey) {
      navigator.clipboard.readText().then(t => t && term.paste(t)).catch(() => {});
      return false;
    }
    return true;
  });

  el.addEventListener('contextmenu', e => {
    e.preventDefault();
    navigator.clipboard.readText().then(t => t && term.paste(t)).catch(() => {});
  });

  // ---- pty -> screen ------------------------------------------------------
  if (host) {
    host.addEventListener('message', ev => {
      const m = ev.data;
      if (typeof m !== 'string') return;
      if (m.startsWith('o:'))      term.write(fromB64(m.slice(2)));
      else if (m === 'focus')      term.focus();
      else if (m.startsWith('x:')) term.write('\r\n\x1b[90m' + m.slice(2) + '\x1b[0m\r\n');
    });
  }

  // fit() needs the font metrics, which are only right once the webfont/layout
  // has settled; do it on the next frame and report the real size then.
  requestAnimationFrame(() => {
    try { fitAddon.fit(); } catch (e) { /* ignore */ }
    cols = term.cols; rows = term.rows;
    term.focus();
    post('ready:' + cols + ',' + rows);
  });
})();
