// Theme and interface language. Both live in Settings, and both have two
// stores: localStorage paints the window before the engine has answered, and
// config.json -- reached through /api/settings -- is the durable one, so the
// choice follows the app rather than this window's profile.
import {post} from '../lib/api.js';
import {app} from '../lib/store.js';
import {setLangCode, t} from '../i18n.js';

const SYS_LIGHT = window.matchMedia ? matchMedia('(prefers-color-scheme: light)') : null;

function resolveTheme(pref) {
  if (pref === 'light' || pref === 'dark') return pref;
  return (SYS_LIGHT && SYS_LIGHT.matches) ? 'light' : 'dark';
}

export function applyTheme(pref, persist) {
  const themePref = (pref === 'light' || pref === 'dark') ? pref : 'system';
  document.documentElement.setAttribute('data-theme', resolveTheme(themePref));
  try { localStorage.setItem('transcriptor-theme', themePref); } catch { /* no store */ }
  app.set({themePref});
  if (persist) post('/api/settings', {ui_theme: themePref});
}
if (SYS_LIGHT && SYS_LIGHT.addEventListener) {
  // "system" tracks the OS setting live.
  SYS_LIGHT.addEventListener('change', () => {
    if (app.get().themePref === 'system') applyTheme('system', false);
  });
}

// What has to be fetched again because the engine words it in the interface
// language: device labels and template names. Set by state/studio.js, which
// would otherwise have to be imported from here in a circle.
let afterLangChange = () => {};
export function onLangChange(fn) { afterLangChange = fn; }

export function setLang(lang, persist) {
  const code = setLangCode(lang);
  document.documentElement.lang = code;
  document.title = t('app.title');
  try { localStorage.setItem('transcriptor-lang', code); } catch { /* no store */ }
  const changed = app.get().lang !== code;
  app.set({lang: code});
  if (persist) post('/api/settings', {ui_language: code});
  if (changed) afterLangChange();
}

// Before the first render, from what this window remembered, so nothing is
// painted in the wrong theme or language and then corrected.
export function paintSavedPrefs() {
  let theme = 'system', lang = 'en';
  try {
    theme = localStorage.getItem('transcriptor-theme') || 'system';
    lang = localStorage.getItem('transcriptor-lang') || 'en';
  } catch { /* the defaults stand */ }
  applyTheme(theme, false);
  setLang(lang, false);
}
