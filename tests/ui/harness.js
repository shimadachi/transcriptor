// Loads the real web/app.js into a vm with mocked browser objects, so the UI
// tests exercise the shipped handlers rather than a copy of them.
//
// Two things are worth knowing when writing tests against this:
//   * `let` bindings in app.js are script-scoped, not properties of the context
//     object, so read them with env.peek('name') rather than off the sandbox.
//   * function declarations DO land on the context, so env.app.poll(),
//     env.app.onBrowserStop() and friends are callable directly.

const fs = require('fs');
const path = require('path');
const vm = require('vm');

function mkEl(id) {
  const el = {
    id, _cls: new Set(), style: {}, dataset: {}, options: [], children: [],
    textContent: '', innerHTML: '', value: '', checked: false, disabled: false,
    hidden: false, selectedIndex: 0, type: '', _listeners: {},
    classList: {
      add: c => el._cls.add(c),
      remove: c => el._cls.delete(c),
      contains: c => el._cls.has(c),
      toggle: (c, on) => {
        const want = on === undefined ? !el._cls.has(c) : !!on;
        want ? el._cls.add(c) : el._cls.delete(c);
      },
    },
    appendChild: c => { el.children.push(c); if (c && c.tagName === 'option') el.options.push(c); return c; },
    insertBefore: () => {}, remove: () => {}, removeAttribute: () => {},
    setAttribute: () => {}, getAttribute: () => null, focus: () => {},
    select: () => {}, pause: () => {}, dispatchEvent: () => {},
    querySelector: () => null, querySelectorAll: () => [],
    addEventListener: (name, fn) => { (el._listeners[name] = el._listeners[name] || []).push(fn); },
    fire: (name, ev) => (el._listeners[name] || []).forEach(fn => fn(ev)),
  };
  // Assigning innerHTML replaces what is inside the element — the mock cannot
  // build nodes out of the string, but it must at least drop the old ones.
  // Leaving them made a second render look like it had appended to the first,
  // which is exactly what the app does to clear a panel before redrawing it.
  let html = '';
  Object.defineProperty(el, 'innerHTML', {
    enumerable: true,
    get: () => html,
    set: (v) => { html = v; el.children.length = 0; el.options.length = 0; },
  });
  return el;
}

// A MediaStreamTrack whose `ended` event the tests can fire by hand.
function mkTrack(kind, media) {
  const t = {
    kind, readyState: 'live', _listeners: {},
    stop() { this.readyState = 'ended'; media.stopped++; },
    addEventListener(name, fn) { (this._listeners[name] = this._listeners[name] || []).push(fn); },
    fire(name) { (this._listeners[name] || []).forEach(fn => fn({})); },
  };
  return t;
}

function mkStream(kinds, media) {
  const tracks = kinds.map(k => mkTrack(k, media));
  return {
    getTracks: () => tracks,
    getAudioTracks: () => tracks.filter(t => t.kind === 'audio'),
    getVideoTracks: () => tracks.filter(t => t.kind === 'video'),
  };
}

function createEnv(root) {
  const els = {};
  const $$ = id => (els[id] = els[id] || mkEl(id));

  const server = {
    state: {
      recording: false, processing: false, phase: 'done', message: 'Done',
      result_rev: 1, summary_rev: 1, has_result: true, has_audio: true,
      device: 'CPU', elapsed: 0, level: 0,
    },
    resultFails: false,
    loadResultCalls: 0,
    // What /api/result hands back. null is "nothing transcribed yet", which is
    // what every test that does not care about the panels wants.
    result: null,
    summary: null,
    // When set, /api/result waits on this promise before answering.
    resultGate: null,
    uploads: [],           // one entry per POST /api/process_file
    uploadFails: false,
    uploadGate: null,
  };

  const media = {stopped: 0, ctxClosed: 0, failAt: null, recorders: []};

  async function fetchMock(url, opts) {
    if (url === '/api/state') {
      return {json: async () => JSON.parse(JSON.stringify(server.state))};
    }
    if (url === '/api/result') {
      server.loadResultCalls++;
      if (server.resultGate) await server.resultGate;
      if (server.resultFails) throw new Error('network down');
      return {json: async () => ({result: server.result, summary: server.summary})};
    }
    if (url === '/api/process_file') {
      const entry = {body: opts && opts.body, done: false};
      server.uploads.push(entry);
      if (server.uploadGate) await server.uploadGate;
      entry.done = true;
      if (server.uploadFails) throw new Error('network down');
      return {json: async () => ({ok: true})};
    }
    if (url === '/api/sources') return {json: async () => ({sources: []})};
    if (url === '/api/settings') return {json: async () => ({templates: [], llm_catalog: []})};
    return {json: async () => ({})};
  }

  class AudioContextMock {
    constructor() { if (media.failAt === 'actx') throw new Error('audio context failed'); }
    createMediaStreamDestination() { return {stream: mkStream(['audio'], media)}; }
    createMediaStreamSource() { return {connect: () => {}}; }
    createAnalyser() {
      return {fftSize: 512, connect: () => {}, getByteTimeDomainData: () => {}};
    }
    close() { media.ctxClosed++; }
  }

  // Follows the W3C recording spec's ordering: an error sets state to inactive
  // and is followed by a final dataavailable and then stop.
  class MediaRecorderMock {
    constructor() {
      if (media.failAt === 'mr') throw new Error('MediaRecorder ctor failed');
      this.state = 'inactive';
      media.recorders.push(this);
    }
    static isTypeSupported() { return true; }
    start() {
      if (media.failAt === 'start') throw new Error('start failed');
      this.state = 'recording';
    }
    stop() {
      if (this.state === 'inactive') return;
      this.state = 'inactive';
      if (this.ondataavailable) this.ondataavailable({data: {size: 5, type: 'audio/webm'}});
      if (this.onstop) this.onstop();
    }
    pause() { this.state = 'paused'; }
    resume() { this.state = 'recording'; }
    // What a real recorder does when its encoder gives up.
    failWithError() {
      this.state = 'inactive';
      if (this.onerror) this.onerror({error: new Error('encoder died')});
      if (this.ondataavailable) this.ondataavailable({data: {size: 5, type: 'audio/webm'}});
      if (this.onstop) this.onstop();
    }
  }

  const document = {
    getElementById: $$,
    createElement: tag => { const e = mkEl(''); e.tagName = tag; return e; },
    querySelector: sel => (sel.includes('csrf') ? {content: 'TESTTOKEN'} : null),
    querySelectorAll: () => [],
    addEventListener: () => {},
    documentElement: {setAttribute: () => {}, style: {}},
    body: mkEl('body'),
  };

  const navigatorMock = {
    mediaDevices: {
      async getUserMedia() {
        if (media.failAt === 'perm') throw new Error('denied');
        return mkStream(['audio'], media);
      },
      async getDisplayMedia() { return mkStream(['audio', 'video'], media); },
    },
  };

  const sandbox = {
    console, document, navigator: navigatorMock, fetch: fetchMock,
    window: {
      matchMedia: null, MediaRecorder: MediaRecorderMock,
      AudioContext: AudioContextMock, addEventListener: () => {},
    },
    MediaRecorder: MediaRecorderMock, AudioContext: AudioContextMock,
    Blob: class {
      constructor(parts) {
        this.size = (parts || []).reduce((n, p) => n + ((p && p.size) || 0), 0);
        this.type = 'audio/webm';
        this._parts = parts || [];
      }
    },
    FormData: class { constructor() { this._f = []; } append(k, v, n) { this._f.push([k, v, n]); } },
    URL: {createObjectURL: () => 'blob:x', revokeObjectURL: () => {}},
    performance: {now: () => Date.now()},
    localStorage: {getItem: () => null, setItem: () => {}},
    location: {hash: ''},
    setTimeout, clearTimeout, setInterval: () => 0, clearInterval: () => {},
    Uint8Array, Math, Date, JSON, Object, Array, String, Number, Promise, Error,
    confirm: () => true, alert: () => {},
    getComputedStyle: () => ({getPropertyValue: () => ''}),
    AbortController: class { constructor() { this.signal = null; } abort() {} },
    // i18n is exercised separately; here a key is its own text.
    t: k => k, LANG: 'en', applyLang: () => {}, STR: {},
  };
  sandbox.globalThis = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(fs.readFileSync(path.join(root, 'web/app.js'), 'utf8'), sandbox,
                  {filename: 'app.js'});

  return {
    els, server, media, app: sandbox,
    peek: expr => vm.runInContext(expr, sandbox),
    poke: stmt => vm.runInContext(stmt, sandbox),
    // Lets pending promise callbacks run.
    settle: (ms = 5) => new Promise(r => setTimeout(r, ms)),
  };
}

module.exports = {createEnv};
