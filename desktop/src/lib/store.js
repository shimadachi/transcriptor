// The interface's state, in one place that is not a component.
//
// Most of what this app does is driven from outside React: a poll every 700ms,
// a recorder's callbacks, a timer watching a download. Those need somewhere to
// put what they learn that does not depend on which component is mounted, and
// they need to read it back synchronously -- "is a take running *now*" cannot
// wait for a render. So the state lives here, the code in src/state/ reads and
// writes it directly, and components subscribe.
import {useSyncExternalStore} from 'react';

export function createStore(initial) {
  let state = initial;
  const listeners = new Set();
  return {
    get: () => state,
    // A shallow merge, always into a new object: a snapshot a component is
    // holding never changes underneath it.
    set(patch) {
      const next = typeof patch === 'function' ? patch(state) : patch;
      state = {...state, ...next};
      listeners.forEach(fn => fn());
    },
    subscribe(fn) {
      listeners.add(fn);
      return () => listeners.delete(fn);
    },
  };
}

export const app = createStore({
  lang: 'en',
  themePref: 'system',
  tab: 'studio',                 // 'studio' | 'library'

  // -- studio ---------------------------------------------------------------
  s: null,                       // the engine's /api/state, as last polled
  // The take as the transport shows it: the engine's, or one recorded in this
  // window, which the engine knows nothing about.
  view: {rec: false, paused: false, elapsed: 0, level: 0},
  recLatch: false,               // Record was pressed; held until a poll answers
  capture: {rec: false, starting: false, paused: false, uploading: false},
  pending: null,                 // {message}: a take the engine has not accepted
  pendingRetrying: false,
  sources: [],                   // the engine's devices
  source: '',
  mic: '',
  templates: [],
  tpl: '',
  ctx: {open: false, title: '', people: '', notes: ''},
  result: null,
  txHasText: false,
  txEmptyKey: 'tx.empty',        // what the empty transcript panel says
  summary: null,
  summaryCut: false,
  live: null,                    // {lines, partial} while the preview has the panel
  liveOverride: null,            // {on}: the switch as just pressed, until a poll

  // -- library --------------------------------------------------------------
  lib: {
    sessions: [], dir: '', loading: true,
    current: null, item: null,
    tx: '', sum: '',             // which saved transcript / summary is on screen
    runKind: '',                 // 'transcribe' | 'summarize' while the form is open
    runError: '', runNote: '',
  },

  // -- chrome ---------------------------------------------------------------
  update: null,                  // the newer release's tag, while its banner is up
  settings: null,                // {focus}: the dialog, while it is open
  toast: {msg: '', on: false},
  ask: null,                     // {title, body, yes, no}
});

export function useApp() {
  return useSyncExternalStore(app.subscribe, app.get);
}

// For a component whose only interest is that the language changed: reading it
// is what makes the t() calls in its render run again.
export function useLang() {
  return useSyncExternalStore(app.subscribe, () => app.get().lang);
}
