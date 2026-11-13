// The page's half of the tray (electron/tray.js is the other).
//
// The tray menu asks the engine what its recorder is doing, but a take
// recorded in this window is invisible to the engine, so the page reports one
// as it starts, pauses and ends -- along with the two settings the tray
// follows, which the page is the first to learn when they are saved.
import {app} from '../lib/store.js';

const shell = window.transcriptorShell || null;

// False on macOS, where there is no tray to put an icon in, so the setting is
// not shown for nothing.
export const hasTray = !!(shell && shell.hasTray);

let trayMode = 'minimize';
let sent = '';

// Only changes are sent: this runs on every poll.
export function syncTray(force) {
  if (!hasTray) return;
  const st = app.get();
  const report = {
    rec: st.capture.rec, paused: st.capture.paused,
    busy: st.capture.starting || st.capture.uploading,
    tray: trayMode, lang: st.lang,
  };
  const key = JSON.stringify(report);
  if (!force && key === sent) return;
  sent = key;
  shell.trayReport(report).catch(() => { sent = ''; });
}

// The saved choice, when settings are read or written.
export function setTrayMode(mode) {
  trayMode = mode || 'minimize';
  syncTray();
}

export function showWindow() {
  if (shell) shell.trayShow();
}

// `handler('record' | 'pause')`. Returns the way to stop listening.
export function onTrayAction(handler) {
  return shell ? shell.onTrayAction(handler) : () => {};
}
