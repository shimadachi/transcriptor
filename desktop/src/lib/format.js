import {getLang} from '../i18n.js';

// The sign leads in Turkish and trails in English.
export function pctText(frac) {
  const n = Math.round(frac * 100);
  return getLang() === 'tr' ? ' (%' + n + ')' : ' (' + n + '%)';
}

// How far along the running job is, or null where the stage cannot say. A
// remote summarizer sends no fraction, and gets no number rather than a
// made-up one.
export function jobFraction(s) {
  return s && s.processing && typeof s.progress === 'number' ? s.progress : null;
}

// h:mm:ss past an hour, m:ss below it -- a two-hour meeting and a two-minute
// note should both read naturally.
export function plClock(sec) {
  if (!isFinite(sec) || sec < 0) sec = 0;
  const s = Math.floor(sec % 60), m = Math.floor(sec / 60) % 60;
  const h = Math.floor(sec / 3600);
  const mm = h ? String(m).padStart(2, '0') : String(m);
  return (h ? h + ':' : '') + mm + ':' + String(s).padStart(2, '0');
}

export function fmtBytes(n) {
  if (!n) return '';
  const u = ['B', 'KB', 'MB', 'GB'];
  let i = 0, v = n;
  while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
  return (v >= 10 || i === 0 ? Math.round(v) : v.toFixed(1)) + ' ' + u[i];
}

// Session ids are "YYYY-MM-DD_HH-MM-SS[_n]"; the folder mtime is the fallback
// for a directory that was not named by us.
export function libDate(s) {
  const m = /^(\d{4})-(\d{2})-(\d{2})_(\d{2})-(\d{2})-(\d{2})/.exec(s.id);
  const d = m ? new Date(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6])
              : (s.mtime ? new Date(s.mtime * 1000) : null);
  if (!d || isNaN(d.getTime())) return s.id;
  return d.toLocaleString(getLang() === 'tr' ? 'tr-TR' : 'en-GB',
    {year: 'numeric', month: 'short', day: 'numeric',
     hour: '2-digit', minute: '2-digit'});
}

// Numeric, component by component, so 0.1.10 correctly beats 0.1.9 -- a string
// compare has it the other way round. A missing component counts as 0, so
// "0.2" and "0.2.0" are the same version, and a non-numeric suffix on a
// component ("0.2.0-rc1") reads as its leading number.
export function cmpVersion(a, b) {
  const pa = String(a).split('.'), pb = String(b).split('.');
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const x = parseInt(pa[i], 10) || 0, y = parseInt(pb[i], 10) || 0;
    if (x !== y) return x < y ? -1 : 1;
  }
  return 0;
}

// A whitespace-only line is not text: whisper hands back a timed, speaker-
// tagged line with nothing in it often enough that counting lines would call an
// empty transcript full.
export function transcriptHasText(res) {
  return !!(res && res.lines &&
            res.lines.some(l => l && typeof l.text === 'string' && l.text.trim()));
}

// The badge shows the device without the symbol the engine leads it with.
export const deviceLabel = device => (device || '').replace(/^[^A-Za-z]+/, '');
