// The update check: the only request this app makes to anything but its own
// engine. GitHub's releases API answers any origin, so the page can ask it
// directly -- no key, no update service, and nothing about the user goes out
// with the request. Every failure path here is silent: a missed update notice
// is not worth an error in someone's face.
import {post} from '../lib/api.js';
import {cmpVersion} from '../lib/format.js';
import {app} from '../lib/store.js';

const UPD_TTL = 24 * 60 * 60 * 1000;      // ask GitHub at most once a day
const UPD_SEEN = 'transcriptor-update';       // {ts, tag} -- the cached answer
const UPD_HID  = 'transcriptor-update-hid';   // the tag the user dismissed

// As last told by the engine's settings, so a save in the settings dialog can
// look again without another round-trip.
let appVersion = '', appRepo = '';
export const versionInfo = () => ({version: appVersion, repo: appRepo});

const read = k => { try { return localStorage.getItem(k); } catch { return null; } };
const write = (k, v) => { try { localStorage.setItem(k, v); } catch { /* no store */ } };

function showUpdate(tag, current) {
  if (!tag || !current || cmpVersion(tag, current) <= 0) return;
  if (read(UPD_HID) === tag) return;          // dismissed, and still the same one
  app.set({update: tag});
}

// `s` is the engine's settings: {check_updates, version, repo}.
export async function checkUpdates(s) {
  if (!s || !s.version) return;
  appVersion = s.version;
  appRepo = s.repo || appRepo;
  if (!s.check_updates) return;

  // A cached answer still raises the banner; only the network call is rationed,
  // which also keeps a shared IP well under GitHub's anonymous cap.
  let seen = null;
  try { seen = JSON.parse(read(UPD_SEEN) || 'null'); } catch { /* ask again */ }
  if (seen && seen.tag && Date.now() - (seen.ts || 0) < UPD_TTL) {
    showUpdate(seen.tag, s.version);
    return;
  }
  if (!appRepo) return;

  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), 8000);
  try {
    const r = await fetch('https://api.github.com/repos/' + appRepo + '/releases/latest',
                          {headers: {'Accept': 'application/vnd.github+json'},
                           signal: ctl.signal, cache: 'no-store'});
    // 403 rate-limited, 404 no releases yet, 5xx -- all just mean "not today".
    if (!r.ok) return;
    // /releases/latest already skips drafts and prereleases. Tags here are bare
    // ("0.1.5"), but tolerate a "v" prefix in case that ever changes.
    const tag = String((await r.json()).tag_name || '').trim().replace(/^v/i, '');
    if (!tag) return;
    // Stamped even when it is not newer, so an up-to-date machine also asks
    // only once a day.
    write(UPD_SEEN, JSON.stringify({ts: Date.now(), tag}));
    showUpdate(tag, s.version);
  } catch {
    /* offline, DNS-blocked, aborted, malformed JSON -- all the same: stay quiet */
  } finally {
    clearTimeout(timer);
  }
}

export function hideUpdateBanner() { app.set({update: null}); }

export function dismissUpdate() {
  const tag = app.get().update;
  if (tag) write(UPD_HID, tag);
  app.set({update: null});
}

// The banner's button. The engine opens its own fixed releases URL, so nothing
// from the page is handed to the OS; if that fails, the URL it names is opened
// the way any link is (refused as a window, passed to the OS by the app).
export async function openReleases() {
  try {
    const r = await post('/api/open_releases');
    if (r && !r.ok && r.url) window.open(r.url, '_blank', 'noopener');
  } catch { /* nothing sensible left to try */ }
}
