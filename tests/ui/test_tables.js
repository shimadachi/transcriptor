// Constants that exist twice, checked against each other.
//
// The clustering threshold is derived from the transcription language by
// Settings::cluster_threshold in src/config.cpp. The settings panel shows which
// value is in force, and it has to show the right one before a save as well as
// after, so web/app.js carries its own copy of the same table. Two copies of a
// table is two copies that can drift, and the drift is invisible: the panel
// simply states a number the diarizer is not using.

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const ROOT = process.argv[2] || path.join(__dirname, '..', '..');

let failures = 0;
function check(name, ok, detail) {
  console.log(`${ok ? 'ok  ' : 'FAIL'}  ${name}${detail ? '  --  ' + detail : ''}`);
  if (!ok) failures++;
}

function read(rel) {
  return fs.readFileSync(path.join(ROOT, rel), 'utf8');
}

// -- the two threshold tables ------------------------------------------------

function uiTable() {
  const m = read('web/app.js').match(/const CLTHR_BY_LANG = (\{[^}]*\})/);
  if (!m) return null;
  return vm.runInNewContext('(' + m[1] + ')');
}

function cppTable() {
  const fn = read('src/config.cpp')
    .match(/float Settings::cluster_threshold\(\)[\s\S]*?\n}/);
  if (!fn) return null;
  const body = fn[0];
  const one = re => {
    const m = body.match(re);
    return m ? parseFloat(m[1]) : null;
  };
  return {
    tr: one(/language == "tr"\) return ([\d.]+)f/),
    en: one(/language == "en"\) return ([\d.]+)f/),
    // The bare return at the end: every language with no row of its own.
    '': one(/return ([\d.]+)f;\s*\n}/),
  };
}

const ui = uiTable();
const cpp = cppTable();

check('web/app.js still declares CLTHR_BY_LANG', ui !== null);
check('src/config.cpp still defines Settings::cluster_threshold', cpp !== null);

if (ui && cpp) {
  for (const lang of ['tr', 'en', '']) {
    const label = lang === '' ? 'auto-detect' : lang;
    check(`threshold for ${label} agrees`,
          cpp[lang] !== null && Math.abs(ui[lang] - cpp[lang]) < 1e-9,
          `panel ${ui[lang]} vs diarizer ${cpp[lang]}`);
  }
  // A language in one table and not the other is the same bug in another shape.
  check('neither table has a language the other lacks',
        Object.keys(ui).sort().join() === Object.keys(cpp).sort().join(),
        `panel [${Object.keys(ui)}] vs diarizer [${Object.keys(cpp)}]`);
}

// -- the string the panel renders --------------------------------------------
// showClthrNote() substitutes the number into set.clthr. A translation that
// dropped the placeholder would render the sentence without its value.

const sandbox = {
  window: {}, console,
  localStorage: {getItem: () => null, setItem: () => {}},
  document: {documentElement: {}, title: '', getElementById: () => null,
             querySelectorAll: () => [], addEventListener: () => {}},
};
sandbox.globalThis = sandbox;
vm.createContext(sandbox);
vm.runInContext(read('web/i18n.js'), sandbox, {filename: 'i18n.js'});
const STR = vm.runInContext('STR', sandbox);

check('set.clthr exists', Boolean(STR && STR['set.clthr']));
if (STR && STR['set.clthr']) {
  for (const lang of ['en', 'tr']) {
    const s = STR['set.clthr'][lang];
    check(`set.clthr[${lang}] keeps the {v} placeholder`,
          typeof s === 'string' && s.includes('{v}'), s);
  }
}

// -- the cancel dialog's strings ---------------------------------------------
// CANCEL_ASK holds i18n keys rather than text, so a typo or a half-finished
// translation shows the user the key itself. Nothing else would catch that:
// t() falls back to the key and the page renders perfectly happily.

const askTable = (() => {
  const m = read('web/app.js').match(/const CANCEL_ASK = (\{[\s\S]*?\n\});/);
  return m ? vm.runInNewContext('(' + m[1] + ')') : null;
})();

check('web/app.js still declares CANCEL_ASK', askTable !== null);
if (askTable && STR) {
  for (const kind of Object.keys(askTable)) {
    for (const slot of ['title', 'body', 'yes', 'no']) {
      const key = askTable[kind][slot];
      const e = key && STR[key];
      check(`cancel/${kind}.${slot} is translated in both languages`,
            Boolean(e && typeof e.en === 'string' && e.en &&
                    typeof e.tr === 'string' && e.tr),
            String(key));
    }
    // "Cancel" as the answer to "shall I cancel?" says nothing. The decline
    // button has to name what carries on instead.
    check(`cancel/${kind} does not answer a cancel question with "Cancel"`,
          askTable[kind].no !== 'set.cancel', askTable[kind].no);
  }
}

check('the toast for a run that finished mid-question is translated',
      Boolean(STR && STR['toast.jobAlreadyDone'] &&
              STR['toast.jobAlreadyDone'].tr));

// -- paths truncate at the front, without losing their leading slash ---------
// direction:rtl is what puts the ellipsis at the front of a path, where the
// session id survives being cut. On its own it also drags the leading slash of
// an absolute path to the far right, so /home/user/Transcriptor renders as
// home/user/Transcriptor/. The <bdi dir="ltr"> inside is what stops that.
// The two only work as a pair: either one alone is a bug, and neither is
// obviously load-bearing to someone tidying up later.

const html = read('web/index.html');
const css = read('web/style.css');

for (const cls of ['lib-dir', 'lib-path']) {
  const rule = css.match(new RegExp('\\.' + cls + '\\s*\\{[^}]*\\}'));
  check(`.${cls} still truncates at the front`,
        Boolean(rule && /direction:\s*rtl/.test(rule[0])));
  // The <bdi> has to be inside that element, not beside it.
  const el = html.match(new RegExp('<code[^>]*class="' + cls + '"[^>]*>([\\s\\S]*?)</code>'));
  check(`.${cls} wraps its text in a <bdi dir="ltr">`,
        Boolean(el && /<bdi[^>]*dir="ltr"[^>]*>/.test(el[1])),
        el ? el[1].trim().slice(0, 46) : 'element not found');
}

// The ids the JS writes to have to be on the <bdi>, or setting textContent
// would replace the wrapper it depends on.
for (const id of ['libDir', 'libPath']) {
  check(`#${id} is the <bdi>, so writing to it keeps the wrapper`,
        new RegExp('<bdi[^>]*id="' + id + '"').test(html));
}

// The setting is gone, so nothing should still be reaching for its input.
check('no leftover reference to the removed threshold input',
      !read('web/app.js').includes('s_clthr') &&
      !read('web/index.html').includes('s_clthr'));
check('the settings POST no longer sends a threshold',
      !read('web/app.js').includes('cluster_threshold:'));

// -- V14: every label names a control ----------------------------------------
// None of the settings labels was tied to its control, so clicking "Separate
// speakers" did nothing and a screen reader announced every field unnamed. A
// label has to wrap its control or point at one by id.
{
  const ids = new Set([...html.matchAll(/\bid="([^"]+)"/g)].map(m => m[1]));
  const loose = [];
  for (const m of html.matchAll(/<label\b([^>]*)>([\s\S]*?)<\/label>/g)) {
    const target = /\bfor="([^"]+)"/.exec(m[1]);
    const wraps = /<(input|select|textarea)\b/.test(m[2]);
    if (!wraps && !(target && ids.has(target[1]))) {
      loose.push((/data-i18n="([^"]+)"/.exec(m[1]) || [])[1] || m[0].slice(0, 40));
    }
  }
  check('V14 every <label> wraps its control or names it with for=',
        loose.length === 0, loose.join(', '));
}

console.log(failures === 0 ? '\ntables: all checks passed'
                           : `\ntables: ${failures} check(s) FAILED`);
process.exit(failures === 0 ? 0 : 1);
