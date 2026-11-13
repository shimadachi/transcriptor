// The notification-area icon. Minimizing the window hides it there, and the
// icon's menu starts, stops and pauses a take without bringing the window back.
//
// Windows and Linux only. macOS has no tray to minimize into -- the Dock is
// where a minimized window goes -- so it gets none, and the setting is not
// offered there (see `hasTray` in preload.cjs).
//
// What the menu shows comes from two places. The engine knows about a take it
// is recording itself and whether a job is running, and is asked once a second.
// A take recorded in the page is invisible to the engine, so the page reports
// those (ipc 'tray:report'), along with the two settings that shape the tray:
// the mode and the interface language.

import {Menu, Tray, nativeImage} from 'electron';

export const TRAY_SUPPORTED = process.platform !== 'darwin';

// Every label in both languages, like every other string the user sees.
const STR = {
  hide:      {en: 'Hide to tray',            tr: 'Tepsiye gizle'},
  show:      {en: 'Show Transcriptor',       tr: "Transcriptor'ı göster"},
  start:     {en: 'Start recording',         tr: 'Kaydı başlat'},
  stop:      {en: 'Stop recording',          tr: 'Kaydı durdur'},
  pause:     {en: 'Pause',                   tr: 'Duraklat'},
  resume:    {en: 'Resume',                  tr: 'Sürdür'},
  quit:      {en: 'Quit',                    tr: 'Çık'},
  tipPaused: {en: 'Transcriptor · paused',   tr: 'Transcriptor · duraklatıldı'},
  tipRec:    {en: 'Transcriptor · recording', tr: 'Transcriptor · kayıtta'},
  tipBusy:   {en: 'Transcriptor · working…', tr: 'Transcriptor · işleniyor…'},
};

// Anything unrecognised is the default, the same rule config.json follows.
export function trayMode(setting) {
  return setting === 'icon' || setting === 'off' ? setting : 'minimize';
}

// Kept free of Electron so the menu's rules can be read (and tested) on their
// own: what is offered, and when each entry can be pressed.
export function trayMenu(status, windowShown) {
  const l = key => STR[key][status.lang === 'tr' ? 'tr' : 'en'];
  return [
    {command: 'showhide', label: l(windowShown ? 'hide' : 'show')},
    {command: 'record', label: l(status.recording ? 'stop' : 'start'),
     enabled: status.canRecord, separatorBefore: true},
    {command: 'pause', label: l(status.paused ? 'resume' : 'pause'),
     enabled: status.recording},
    {command: 'quit', label: l('quit'), separatorBefore: true},
  ];
}

export function trayTooltip(status) {
  const l = key => STR[key][status.lang === 'tr' ? 'tr' : 'en'];
  if (status.recording) return l(status.paused ? 'tipPaused' : 'tipRec');
  if (!status.canRecord) return l('tipBusy');
  return 'Transcriptor';
}

// `engineState()` resolves to the engine's /api/state, or rejects.
// `onCommand('record' | 'pause' | 'quit')` is what a menu entry asks for.
export function createTray({win, iconPath, engineState, onCommand}) {
  if (!TRAY_SUPPORTED) return {report() {}, show() {}, destroy() {}};

  const icon = nativeImage.createFromPath(iconPath);
  let tray = null;
  let mode = 'minimize', lang = 'en';
  let page = {rec: false, paused: false, busy: false};   // a take in the page
  let engine = {recording: false, paused: false, processing: false};
  let drawn = '';

  const status = () => {
    const s = {
      recording: engine.recording,
      paused: engine.recording && engine.paused,
      canRecord: !engine.processing,
      lang,
    };
    if (page.rec) { s.recording = true; s.paused = page.paused; }
    if (page.busy) s.canRecord = false;
    return s;
  };

  const shown = () => !win.isDestroyed() && win.isVisible() && !win.isMinimized();

  const show = () => {
    if (win.isDestroyed()) return;
    if (win.isMinimized()) win.restore();
    win.show();
    win.focus();
  };

  const run = command => {
    if (command === 'showhide') { if (shown()) win.hide(); else show(); paint(); }
    else onCommand(command);
  };

  function paint() {
    if (mode === 'off' || icon.isEmpty()) {
      if (tray) { tray.destroy(); tray = null; drawn = ''; }
      return;
    }
    if (!tray) {
      tray = new Tray(icon);
      tray.on('click', () => run('showhide'));
    }
    const s = status();
    const items = trayMenu(s, shown());
    const tip = trayTooltip(s);
    // Only when something changed: on Linux setting the menu again closes one
    // that is open, and this runs every second.
    const key = JSON.stringify([items, tip]);
    if (key === drawn) return;
    drawn = key;
    const template = [];
    for (const it of items) {
      if (it.separatorBefore) template.push({type: 'separator'});
      template.push({label: it.label, enabled: it.enabled !== false,
                     click: () => run(it.command)});
    }
    tray.setToolTip(tip);
    tray.setContextMenu(Menu.buildFromTemplate(template));
  }

  // "Minimize to the tray" is the window leaving the taskbar as it minimizes.
  // Without an icon to bring it back there is nowhere to hide it to.
  win.on('minimize', () => {
    if (mode === 'minimize' && tray) { win.hide(); paint(); }
  });
  win.on('show', paint);
  win.on('hide', paint);
  win.on('restore', paint);

  const tick = async () => {
    try {
      const s = await engineState();
      engine = {recording: !!s.recording, paused: !!s.paused, processing: !!s.processing};
    } catch { /* the engine is busy or gone; keep what was last known */ }
    paint();
  };
  const timer = setInterval(tick, 1000);
  tick();

  return {
    // From the page: its own take, and the settings the tray follows.
    report(r) {
      const flag = k => !!(r && r[k] === true);
      page = {rec: flag('rec'), paused: flag('paused'), busy: flag('busy')};
      if (r && typeof r.tray === 'string') mode = trayMode(r.tray);
      if (r && typeof r.lang === 'string') lang = r.lang === 'tr' ? 'tr' : 'en';
      tick();
    },
    show,
    destroy() {
      clearInterval(timer);
      if (tray) { tray.destroy(); tray = null; }
    },
  };
}
