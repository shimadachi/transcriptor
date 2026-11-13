import {useEffect} from 'react';

import {t} from '../i18n.js';
import {api} from '../lib/api.js';
import {deviceLabel} from '../lib/format.js';
import {app, useApp} from '../lib/store.js';
import {loadLibrary, pauseLibraryAudio} from '../state/library.js';
import {applyTheme, setLang} from '../state/prefs.js';
import {openSettings} from '../state/settings.js';
import {POLL_MS, loadSources, loadTemplates, poll, trayAction} from '../state/studio.js';
import {onTrayAction, setTrayMode} from '../state/tray.js';
import {checkUpdates, dismissUpdate, openReleases} from '../state/updates.js';
import {AskDialog, Toast} from './Dialogs.jsx';
import {Library} from './Library.jsx';
import {SettingsDialog} from './Settings.jsx';
import {Studio} from './Studio.jsx';

function showTab(name) {
  app.set({tab: name});
  // Nothing should keep playing behind a hidden tab; the position is kept.
  if (name === 'library') loadLibrary(); else pauseLibraryAudio();
}

// Once, when the window opens: what the engine knows that the page does not.
async function start() {
  loadSources();
  loadTemplates();
  // config.json is the durable store: reconcile what this window remembered
  // with it, so the choice follows the app rather than the window's profile.
  try {
    const s = await api('/api/settings');
    const now = app.get();
    if (s.ui_language && s.ui_language !== now.lang) setLang(s.ui_language, false);
    if (s.ui_theme && s.ui_theme !== now.themePref) applyTheme(s.ui_theme, false);
    setTrayMode(s.tray);
    checkUpdates(s);   // deliberately not awaited: never hold up the first paint
  } catch { /* the engine is not ready yet; the saved values stand */ }
}

// Design-preview hooks, reached only by starting the app with a hash
// (TRANSCRIPTOR_START_HASH): each opens the window on one particular state.
function previewFromHash() {
  const hash = window.location.hash;
  if (hash === '#library') showTab('library');
  if (hash === '#settings') openSettings();
  if (hash === '#update') app.set({update: '9.9.9'});
}

export function App() {
  const st = useApp();
  const s = st.s || {};

  useEffect(() => {
    start();
    previewFromHash();
    poll();
    const timer = setInterval(poll, POLL_MS);
    const offTray = onTrayAction(trayAction);
    return () => { clearInterval(timer); offTray(); };
  }, []);

  const lib = st.tab === 'library';
  return (
    <>
      <div className="wrap">
        <header>
          <div className="brand">
            <span className="mark">transcript<em>or</em></span>
          </div>
          <span className="spacer"></span>
          <span id="badge" className="chip">
            <span id="led" className={'led' + (s.cuda ? '' : ' cpu')}></span>
            <span id="badgeText">{st.s ? deviceLabel(s.device) : '…'}</span>
          </span>
          <button id="settingsBtn" className="ghost mini"
                  onClick={() => openSettings()}>{t('hdr.settings')}</button>
        </header>

        {/* Shown only when GitHub reports a release newer than this build. */}
        {st.update && (
          <div id="updBar" className="updbar" style={{display: 'flex'}}>
            <span className="updbar-k">{t('upd.k')}</span>
            <span className="updbar-txt"><span>{t('upd.new')}</span>{' '}
              <b id="updVer">{st.update}</b></span>
            <button id="updGet" className="mini ok" type="button"
                    onClick={openReleases}>{t('upd.get')}</button>
            <button id="updHide" className="ghost mini" type="button" title={t('upd.hideTitle')}
                    onClick={dismissUpdate}>{t('upd.hide')}</button>
          </div>
        )}

        <nav className="tabs" role="tablist">
          <button id="tabStudio" className={'tab' + (lib ? '' : ' on')} type="button" role="tab"
                  aria-selected={!lib} aria-controls="viewStudio"
                  onClick={() => showTab('studio')}>{t('tab.studio')}</button>
          <button id="tabLibrary" className={'tab' + (lib ? ' on' : '')} type="button" role="tab"
                  aria-selected={lib} aria-controls="viewLibrary"
                  onClick={() => showTab('library')}>{t('tab.library')}</button>
        </nav>

        <Studio hidden={lib}/>
        <Library hidden={!lib}/>
      </div>

      <SettingsDialog/>
      <AskDialog/>
      <Toast/>
    </>
  );
}
