// The engine: the C++ process that records, transcribes and summarizes.
//
// It is started here, as a child, and it is the only thing this app talks to.
// Two pipes carry the whole relationship. On its stdout it prints one line,
//   transcriptor-ready {"port": N, "token": "...", "version": "..."}
// and that token never leaves this process: the proxy in main.js adds it to
// each request, so the page cannot leak what it was never given. Its stdin is
// held open and never written to; the engine reads it only to find out when we
// are gone, which covers a crash here as well as a quit.

import {spawn} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const READY = 'transcriptor-ready ';
// Starting is opening a config file and binding a socket. Far longer than that
// needs, and short enough that a hung engine is not a blank screen for ever.
const START_TIMEOUT_MS = 30000;
// What a quit waits for. The engine may be finishing a file it is writing;
// past this it is killed, because an app that will not close is worse.
const STOP_TIMEOUT_MS = 8000;

const here = path.dirname(fileURLToPath(import.meta.url));

// Where the engine may be, most deliberate first: named outright, shipped
// beside the app, or built in the repository this folder sits in.
export function engineCandidates(resourcesPath) {
  const exe = process.platform === 'win32' ? 'transcriptor.exe' : 'transcriptor';
  const out = [];
  if (process.env.TRANSCRIPTOR_ENGINE) out.push(process.env.TRANSCRIPTOR_ENGINE);
  if (resourcesPath) out.push(path.join(resourcesPath, 'engine', exe));

  const build = path.join(here, '..', '..', 'build');
  let presets = [];
  try { presets = fs.readdirSync(build); } catch { /* not a checkout */ }
  const built = [];
  for (const preset of presets) {
    for (const rel of [exe, path.join('Release', exe),
                       path.join('transcriptor.app', 'Contents', 'MacOS', 'transcriptor')]) {
      const file = path.join(build, preset, rel);
      try {
        const st = fs.statSync(file);
        if (st.isFile()) built.push({file, mtime: st.mtimeMs});
      } catch { /* not this one */ }
    }
  }
  // Several presets may have been built; the one built last is the one being
  // worked on.
  built.sort((a, b) => b.mtime - a.mtime);
  return out.concat(built.map(b => b.file));
}

export function findEngine(resourcesPath) {
  for (const file of engineCandidates(resourcesPath)) {
    try { if (fs.statSync(file).isFile()) return file; } catch { /* next */ }
  }
  return null;
}

// Resolves once the engine has said where it is listening. `onExit` is called
// if it goes away afterwards without having been asked to.
export function startEngine(file, {onExit} = {}) {
  return new Promise((resolve, reject) => {
    let child;
    try {
      // Port 0: any free one. The configured port is for nobody now that the
      // window does not load from it, and a fixed one is a way to fail.
      child = spawn(file, ['--port', '0'], {
        stdio: ['pipe', 'pipe', 'inherit'],
        windowsHide: true,
      });
    } catch (e) { reject(e); return; }

    let ready = false, stopping = false, buffered = '';
    const exited = new Promise(done => child.once('exit', done));

    const timer = setTimeout(() => {
      if (ready) return;
      child.kill();
      reject(new Error('the engine did not start in time'));
    }, START_TIMEOUT_MS);

    const stop = async () => {
      stopping = true;
      if (child.exitCode !== null || child.signalCode !== null) return;
      // Closing its stdin is the request; the engine shuts down in order.
      try { child.stdin.end(); } catch { /* already closed */ }
      const gave_up = new Promise(done => setTimeout(() => done('timeout'), STOP_TIMEOUT_MS));
      if (await Promise.race([exited, gave_up]) === 'timeout') child.kill();
    };

    child.stdout.setEncoding('utf8');
    child.stdout.on('data', chunk => {
      if (ready) return;
      buffered += chunk;
      let nl;
      while ((nl = buffered.indexOf('\n')) >= 0) {
        const line = buffered.slice(0, nl).trim();
        buffered = buffered.slice(nl + 1);
        if (!line.startsWith(READY)) continue;
        let info = null;
        try { info = JSON.parse(line.slice(READY.length)); } catch { /* below */ }
        if (!info || !Number.isInteger(info.port) || typeof info.token !== 'string') {
          child.kill();
          reject(new Error('the engine announced itself in a form this app cannot read'));
          return;
        }
        ready = true;
        clearTimeout(timer);
        resolve({port: info.port, token: info.token, version: info.version || '', stop});
        return;
      }
    });
    // The engine dying first makes this end of the pipe error; that is
    // reported through 'exit' below, and must not be an uncaught exception.
    child.stdin.on('error', () => {});

    child.once('error', e => { clearTimeout(timer); if (!ready) reject(e); });
    child.once('exit', (code, signal) => {
      clearTimeout(timer);
      if (!ready) {
        reject(new Error('the engine exited before it was ready (' +
                         (signal || 'code ' + code) + ')'));
      } else if (!stopping && onExit) {
        onExit(code, signal);
      }
    });
  });
}
