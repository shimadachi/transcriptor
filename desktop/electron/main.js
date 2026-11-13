// The desktop app: one window over the local engine.
//
// Nothing here listens on a port and there is no page a browser could open.
// The window loads the built React UI from this app's own files, under a
// private scheme, and its requests to /api/* are forwarded to the engine by
// this process -- which is where the engine's token is added. The engine
// refuses anything without it, so the window is the only way in.

import {app, BrowserWindow, Menu, desktopCapturer, dialog, ipcMain, net,
        protocol, session, shell} from 'electron';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';

import {findEngine, startEngine} from './engine.js';
import {TRAY_SUPPORTED, createTray} from './tray.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const DIST = path.join(here, '..', 'dist');

const SCHEME = 'app';
const HOST = 'transcriptor';
const ORIGIN = SCHEME + '://' + HOST;
const TOKEN_HEADER = 'X-Transcriptor-Token';

// Before the engine has answered there is no interface language to follow, so
// the few things said this early go by the system's.
const L = (en, tr) => (app.getLocale().toLowerCase().startsWith('tr') ? tr : en);

// `standard` gives the page a real origin (relative URLs, localStorage);
// `secure` is what getUserMedia and getDisplayMedia insist on; `stream` lets
// the <audio> element seek through a recording instead of buffering all of it.
protocol.registerSchemesAsPrivileged([{
  scheme: SCHEME,
  privileges: {standard: true, secure: true, supportFetchAPI: true, stream: true},
}]);

let engine = null;
let win = null;
let tray = null;
let quitting = false;

// ---- the window's requests ----

// What is passed on to the engine. Everything else the page's request carries
// stays here -- its Origin in particular: the engine refuses a POST that names
// an origin other than loopback, and this request is ours, not the page's.
const FORWARDED = ['accept', 'content-type', 'range', 'if-range'];

function forwardToEngine(request, url) {
  const headers = new Headers();
  for (const name of FORWARDED) {
    const value = request.headers.get(name);
    if (value) headers.set(name, value);
  }
  headers.set(TOKEN_HEADER, engine.token);

  const init = {method: request.method, headers, bypassCustomProtocolHandlers: true};
  if (request.method !== 'GET' && request.method !== 'HEAD' && request.body) {
    // Streamed through, never held: an upload may be hours of audio.
    init.body = request.body;
    init.duplex = 'half';
  }
  return net.fetch('http://127.0.0.1:' + engine.port + url.pathname + url.search, init);
}

function serveFile(url) {
  let rel;
  try { rel = decodeURIComponent(url.pathname); } catch { rel = ''; }
  if (rel === '' || rel === '/') rel = '/index.html';
  const file = path.resolve(DIST, '.' + rel);
  // Only what the build put in dist/. resolve() has already folded any `..`,
  // so a path that climbed out no longer starts with the folder.
  if (!file.startsWith(DIST + path.sep)) return new Response('not found', {status: 404});
  return net.fetch(pathToFileURL(file).toString(), {bypassCustomProtocolHandlers: true})
    .catch(() => new Response('not found', {status: 404}));
}

function handleAppRequest(request) {
  let url;
  try { url = new URL(request.url); } catch { return new Response('bad request', {status: 400}); }
  if (url.host !== HOST) return new Response('not found', {status: 404});
  if (url.pathname.startsWith('/api/')) {
    if (!engine) return new Response('the engine is not running', {status: 503});
    return forwardToEngine(request, url).catch(
      () => Response.json({error: L('The engine is not answering.',
                                    'Motor yanıt vermiyor.')}, {status: 502}));
  }
  return serveFile(url);
}

function engineState() {
  return fetch('http://127.0.0.1:' + engine.port + '/api/state',
               {headers: {[TOKEN_HEADER]: engine.token}}).then(r => r.json());
}

// ---- what the page may do ----

const fromApp = contents => {
  try { return new URL(contents.getURL()).origin === ORIGIN; } catch { return false; }
};

// http, https and mailto only: the schemes that just navigate. The same rule
// the engine holds a summary's links to before it hands one to the OS.
function openExternally(target) {
  let url;
  try { url = new URL(target); } catch { return; }
  if (['http:', 'https:', 'mailto:'].includes(url.protocol)) shell.openExternal(url.toString());
}

function setUpSession() {
  const ses = session.defaultSession;
  ses.protocol.handle(SCHEME, handleAppRequest);

  // The microphone and the screen picker, for the takes the page records
  // itself, and for this app's own page alone.
  const allowed = new Set(['media', 'display-capture']);
  ses.setPermissionRequestHandler((contents, permission, done) => {
    done(fromApp(contents) && allowed.has(permission));
  });
  ses.setPermissionCheckHandler((contents, permission, origin) => {
    return origin === ORIGIN && allowed.has(permission);
  });

  // "System audio" in the page asks for the screen, because that is the only
  // door a page has to what the machine is playing; the video is thrown away
  // as soon as it arrives. Loopback audio is something Chromium only offers on
  // some platforms -- where it does not, the engine's own sources are the way
  // to record the system, and the page says there was no audio in the capture.
  ses.setDisplayMediaRequestHandler(async (request, done) => {
    try {
      const sources = await desktopCapturer.getSources(
        {types: ['screen'], thumbnailSize: {width: 0, height: 0}});
      if (!sources.length) { done({}); return; }
      const grant = {video: sources[0]};
      if (process.platform !== 'linux') grant.audio = 'loopback';
      done(grant);
    } catch {
      done({});
    }
  });
}

// ---- the window ----

function iconPath() {
  const candidates = [
    path.join(process.resourcesPath || '', 'logo-256.png'),
    path.join(here, '..', '..', 'assets', 'logo-256.png'),
  ];
  return candidates.find(f => { try { return fs.statSync(f).isFile(); } catch { return false; } })
    || '';
}

function createWindow() {
  const icon = iconPath();
  win = new BrowserWindow({
    width: 1280, height: 860, minWidth: 900, minHeight: 600,
    title: L('Transcriptor · audio · text · summary', 'Transcriptor · ses · metin · özet'),
    // Shown once the page has painted, so the window never opens on a blank
    // rectangle in the wrong theme.
    show: false,
    backgroundColor: '#14110d',
    icon: icon || undefined,
    webPreferences: {
      preload: path.join(here, 'preload.cjs'),
      contextIsolation: true,
      sandbox: true,
      nodeIntegration: false,
      spellcheck: false,
      additionalArguments: TRAY_SUPPORTED ? ['--transcriptor-tray'] : [],
    },
  });

  win.once('ready-to-show', () => win.show());

  // The window is the app and never anything else: a link opens in the real
  // browser, and nothing navigates this page away from itself.
  win.webContents.setWindowOpenHandler(({url}) => { openExternally(url); return {action: 'deny'}; });
  win.webContents.on('will-navigate', (event, url) => {
    let same = false;
    try { same = new URL(url).origin === ORIGIN; } catch { /* not ours */ }
    if (!same) { event.preventDefault(); openExternally(url); }
  });

  tray = createTray({
    win,
    iconPath: icon,
    engineState,
    onCommand: command => {
      if (command === 'quit') app.quit();
      else if (!win.isDestroyed()) win.webContents.send('tray:action', command);
    },
  });

  win.on('closed', () => { win = null; });
  win.loadURL(ORIGIN + '/index.html' + (process.env.TRANSCRIPTOR_START_HASH || ''));
  if (process.env.TRANSCRIPTOR_DEVTOOLS) win.webContents.openDevTools({mode: 'detach'});
}

function setUpIpc() {
  // Only this app's own page is listened to.
  const ours = event => {
    try { return new URL(event.senderFrame.url).origin === ORIGIN; } catch { return false; }
  };
  ipcMain.handle('tray:report', (event, report) => {
    if (ours(event) && tray) tray.report(report);
  });
  ipcMain.handle('tray:show', event => {
    if (ours(event) && tray) tray.show();
  });
}

// ---- start and stop ----

function fail(message) {
  dialog.showErrorBox('Transcriptor', message);
  quitting = true;
  app.exit(1);
}

async function start() {
  const file = findEngine(app.isPackaged ? process.resourcesPath : '');
  if (!file) {
    fail(L('The Transcriptor engine was not found. Build it first (cmake --build --preset <name>), '
           + 'or point TRANSCRIPTOR_ENGINE at the binary.',
           'Transcriptor motoru bulunamadı. Önce derleyin (cmake --build --preset <ad>) '
           + 'ya da TRANSCRIPTOR_ENGINE değişkenini dosyaya yönlendirin.'));
    return;
  }
  try {
    engine = await startEngine(file, {
      onExit: () => {
        if (quitting) return;
        fail(L('The Transcriptor engine stopped unexpectedly. The app will close; '
               + 'recordings already saved are in the output folder.',
               'Transcriptor motoru beklenmedik biçimde durdu. Uygulama kapanacak; '
               + 'kaydedilmiş kayıtlar çıktı klasöründedir.'));
      },
    });
  } catch (e) {
    fail(L('The Transcriptor engine could not be started: ',
           'Transcriptor motoru başlatılamadı: ') + (e && e.message ? e.message : e));
    return;
  }
  setUpSession();
  setUpIpc();
  createWindow();
}

// One engine means one app: two would share a config file and an output
// folder, and fight over the microphone.
if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', () => {
    if (!win) return;
    if (win.isMinimized()) win.restore();
    win.show();
    win.focus();
  });

  // No menu bar on Windows and Linux; the page is the whole interface. macOS
  // keeps the default one, which is where its copy and paste live.
  if (process.platform !== 'darwin') Menu.setApplicationMenu(null);

  app.whenReady().then(start);

  // Closing the window is leaving the app, on every platform: a recorder that
  // stays behind with no window is not something to do without being asked.
  app.on('window-all-closed', () => app.quit());

  // The engine is given the chance to finish what it is writing before this
  // process, and the pipe it is watching, go away.
  app.on('before-quit', event => {
    if (quitting || !engine) return;
    event.preventDefault();
    quitting = true;
    if (tray) tray.destroy();
    engine.stop().finally(() => app.quit());
  });
}
