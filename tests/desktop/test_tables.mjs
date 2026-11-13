// Tables the desktop app holds, checked against what they have to agree with.
//
// Node builtins only, and nothing from desktop/node_modules: the modules read
// here are plain ES modules with no imports of their own, so this runs on a
// machine that has never installed the desktop app's dependencies.
//
// V49: the clustering threshold is derived from the transcription language by
// Settings::cluster_threshold in src/config.cpp. The settings dialog shows
// which value is in force, and it has to show the right one before a save as
// well as after, so desktop/src/lib/tables.js carries its own copy of the same
// table. Two copies of a table is two copies that can drift, and the drift is
// invisible: the dialog simply states a number the diarizer is not using.
//
// D1: every string the interface shows is English and Turkish. A key with no
// entry renders as the key itself, and one with no `tr` as English in a
// Turkish window; nothing else would catch either.

import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';

const ROOT = process.argv[2] || path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = rel => fs.readFileSync(path.join(ROOT, rel), 'utf8');
const load = rel => import(pathToFileURL(path.join(ROOT, rel)).href);

let failures = 0;
function check(name, ok, detail) {
  console.log(`${ok ? 'ok  ' : 'FAIL'}  ${name}${detail ? '  --  ' + detail : ''}`);
  if (!ok) failures++;
}

const {CLTHR_BY_LANG, CANCEL_ASK, BROWSER_SOURCES} = await load('desktop/src/lib/tables.js');
const {STR} = await load('desktop/src/i18n.js');

// -- the two threshold tables -------------------------------------------------
function cppTable() {
  const fn = read('src/config.cpp')
    .match(/float Settings::cluster_threshold\(\)[\s\S]*?\n}/);
  if (!fn) return null;
  const one = re => { const m = fn[0].match(re); return m ? parseFloat(m[1]) : null; };
  return {
    tr: one(/language == "tr"\) return ([\d.]+)f/),
    en: one(/language == "en"\) return ([\d.]+)f/),
    // The bare return at the end: every language with no row of its own.
    '': one(/return ([\d.]+)f;\s*\n}/),
  };
}
const cpp = cppTable();
check('V49 src/config.cpp still defines Settings::cluster_threshold', cpp !== null);
if (cpp) {
  for (const lang of ['tr', 'en', '']) {
    check(`V49 threshold for ${lang || 'auto-detect'} agrees`,
          CLTHR_BY_LANG[lang] === cpp[lang], `page ${CLTHR_BY_LANG[lang]}, engine ${cpp[lang]}`);
  }
  check('V49 neither table has a language the other lacks',
        Object.keys(CLTHR_BY_LANG).sort().join() === Object.keys(cpp).sort().join());
}
check('V49 set.clthr keeps the {v} placeholder in both languages',
      ['en', 'tr'].every(l => STR['set.clthr'] && STR['set.clthr'][l].includes('{v}')));

// -- every string, in both languages -------------------------------------------
const untranslated = Object.entries(STR)
  .filter(([, e]) => typeof e.en !== 'string' || typeof e.tr !== 'string' || !e.en || !e.tr)
  .map(([k]) => k);
check('D1 every string has English and Turkish', untranslated.length === 0,
      untranslated.slice(0, 8).join(', '));

// Every key the source asks for by name exists. Keys built at run time
// (t(cond ? 'a' : 'b')) are caught too, since each literal is matched alone.
function sources(dir) {
  return fs.readdirSync(path.join(ROOT, dir), {withFileTypes: true}).flatMap(e =>
    e.isDirectory() ? sources(path.join(dir, e.name))
    : /\.(js|jsx)$/.test(e.name) ? [path.join(dir, e.name)] : []);
}
const asked = new Map();
const KEY = /'((?:app|hdr|tab|upd|rec|src|st|btn|saved|pending|note|tx|live|sum|lib|pl|set|stt|llm|toast|ask)\.[A-Za-z0-9.]+)'/g;
for (const file of sources('desktop/src')) {
  if (file.endsWith('i18n.js')) continue;
  for (const m of read(file).matchAll(KEY)) {
    if (!asked.has(m[1])) asked.set(m[1], file);
  }
}
const missing = [...asked].filter(([k]) => !STR[k]).map(([k, f]) => `${k} (${path.basename(f)})`);
check('D1 the interface asks for a fair number of strings', asked.size > 150, String(asked.size));
check('D1 every string the interface asks for exists', missing.length === 0, missing.join(', '));

// -- the cancel questions ------------------------------------------------------
for (const [kind, q] of Object.entries(CANCEL_ASK)) {
  for (const slot of ['title', 'body', 'yes', 'no']) {
    check(`cancel/${kind}.${slot} is translated in both languages`,
          Boolean(STR[q[slot]] && STR[q[slot]].en && STR[q[slot]].tr), q[slot]);
  }
  // A dialog about cancelling whose "no" reads "Cancel" has two buttons that
  // both say cancel.
  check(`cancel/${kind} does not answer a cancel question with "Cancel"`,
        q.no !== 'set.cancel' && STR[q.no] && STR[q.no].en !== STR['set.cancel'].en);
}
check('the window\'s own sources are named in both languages',
      BROWSER_SOURCES.every(b => STR[b.key] && STR[b.key].en && STR[b.key].tr));

// -- the tray -----------------------------------------------------------------
// electron/tray.js imports Electron, so it is read rather than loaded.
{
  const tray = read('desktop/electron/tray.js');
  const table = tray.match(/const STR = \{([\s\S]*?)\n\};/);
  check('desktop/electron/tray.js still has its string table', table !== null);
  if (table) {
    const rows = table[1].split('\n').filter(l => l.trim());
    const bad = rows.filter(l => !/\ben:\s*['"]/.test(l) || !/\btr:\s*['"]/.test(l));
    check('D1 every tray label has English and Turkish', rows.length >= 8 && bad.length === 0,
          bad.join(' | '));
  }
  // The choices Settings offers are the ones the engine keeps and the tray
  // reads: a value one side does not know falls back to the default silently.
  const config = read('src/config.cpp').match(/if \(tray != "(\w+)" && tray != "(\w+)"\) tray = "(\w+)";/);
  const mode = tray.match(/setting === '(\w+)' \|\| setting === '(\w+)' \? setting : '(\w+)'/);
  const offered = [...read('desktop/src/components/Settings.jsx')
    .matchAll(/\{value: '(\w+)', label: t\('set\.tray\w+'\)\}/g)].map(m => m[1]).sort().join();
  check('src/config.cpp still validates the tray setting', config !== null);
  check('tray.js still reads the tray setting', mode !== null);
  if (config && mode) {
    const norm = m => [m[1], m[2]].sort().join() + '|' + m[3];
    check('the engine and the tray accept the same tray modes', norm(config) === norm(mode),
          norm(config) + ' vs ' + norm(mode));
    check('Settings offers exactly those modes',
          offered === [config[1], config[2], config[3]].sort().join(), offered);
  }
}

console.log(failures === 0 ? '\ntables: all checks passed'
                           : `\ntables: ${failures} check(s) FAILED`);
process.exit(failures === 0 ? 0 : 1);
