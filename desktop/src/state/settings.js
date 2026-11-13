import {api} from '../lib/api.js';
import {app} from '../lib/store.js';
import {t} from '../i18n.js';
import {toast} from './ui.js';

// Opens the settings dialog. `focus` names a control to land on rather than
// just the tab it lives in -- the studio's "no speech model" caution uses it.
export function openSettings(focus) {
  app.set({settings: {focus: typeof focus === 'string' ? focus : ''}});
}

export function closeSettings() {
  app.set({settings: null});
}

// ---- downloadable models ----
// Speech weights and summarizer GGUFs behave identically from the dialog: pick
// from a catalog that says what each one is and what it costs, press Download,
// watch it land. The engine runs one download at a time, so one timer serves
// all of them.
//
// The timer lives here rather than in the dialog because a download outlives
// it: closing Settings does not stop the fetch, and its ending is still worth
// a toast. While the dialog is open it listens in, to move its own controls.

// The sections of the dialog that have download controls. The speaker models
// and the voice detector are fetched from the studio's cautions and report
// themselves there.
export const DL_KINDS = ['llm', 'whisper', 'live'];

let dlTimer = null;
let dlKind = 'llm';       // which section the running download belongs to
let dlListener = null;

export function setDownloadKind(kind) { dlKind = kind; }
export function listenToDownload(fn) { dlListener = fn; }

// Poll until the download thread finishes. `phase` is 'active', 'error' or
// 'done'; 'done' carries the settings as they are afterwards, because the
// engine has already pointed them at the new file.
export function pollDownload() {
  if (dlTimer) return;
  const tell = event => { if (dlListener) dlListener(event); };
  const stop = () => { clearInterval(dlTimer); dlTimer = null; };
  dlTimer = setInterval(async () => {
    let d;
    try { d = await api('/api/model/download'); } catch { return; }
    const kind = d.kind || dlKind;
    if (!DL_KINDS.includes(kind)) {
      if (!d.active) stop();
      return;
    }
    if (d.active) { tell({kind, phase: 'active', d}); return; }
    stop();
    if (d.error) {
      tell({kind, phase: 'error', d});
      // A download the user stopped is not a failure.
      toast(d.cancelled ? t('toast.dlCancelled') : t('toast.dlFailed'));
      return;
    }
    toast(t('toast.dlDone'));
    let s = null;
    try { s = await api('/api/settings'); } catch { /* the dialog keeps what it has */ }
    tell({kind, phase: 'done', d, s});
  }, 1000);
}
