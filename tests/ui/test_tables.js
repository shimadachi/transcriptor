// Constants that exist twice, checked against each other.
//
// V49: the clustering threshold is derived from the transcription language by
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

check('V49 web/app.js still declares CLTHR_BY_LANG', ui !== null);
check('V49 src/config.cpp still defines Settings::cluster_threshold',
      cpp !== null);

if (ui && cpp) {
  for (const lang of ['tr', 'en', '']) {
    const label = lang === '' ? 'auto-detect' : lang;
    check(`V49 threshold for ${label} agrees`,
          cpp[lang] !== null && Math.abs(ui[lang] - cpp[lang]) < 1e-9,
          `panel ${ui[lang]} vs diarizer ${cpp[lang]}`);
  }
  // A language in one table and not the other is the same bug in another shape.
  check('V49 neither table has a language the other lacks',
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

check('V49 set.clthr exists', Boolean(STR && STR['set.clthr']));
if (STR && STR['set.clthr']) {
  for (const lang of ['en', 'tr']) {
    const s = STR['set.clthr'][lang];
    check(`V49 set.clthr[${lang}] keeps the {v} placeholder`,
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

check('V26 the toast for a run that finished mid-question is translated',
      Boolean(STR && STR['toast.jobAlreadyDone'] &&
              STR['toast.jobAlreadyDone'].tr));

// -- V52: paths truncate at the front, without losing their leading slash ----
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
  check(`V52 .${cls} still truncates at the front`,
        Boolean(rule && /direction:\s*rtl/.test(rule[0])));
  // The <bdi> has to be inside that element, not beside it.
  const el = html.match(new RegExp('<code[^>]*class="' + cls + '"[^>]*>([\\s\\S]*?)</code>'));
  check(`V52 .${cls} wraps its text in a <bdi dir="ltr">`,
        Boolean(el && /<bdi[^>]*dir="ltr"[^>]*>/.test(el[1])),
        el ? el[1].trim().slice(0, 46) : 'element not found');
}

// The ids the JS writes to have to be on the <bdi>, or setting textContent
// would replace the wrapper it depends on.
for (const id of ['libDir', 'libPath']) {
  check(`V52 #${id} is the <bdi>, so writing to it keeps the wrapper`,
        new RegExp('<bdi[^>]*id="' + id + '"').test(html));
}

// The setting is gone, so nothing should still be reaching for its input.
check('V49 no leftover reference to the removed threshold input',
      !read('web/app.js').includes('s_clthr') &&
      !read('web/index.html').includes('s_clthr'));
check('V49 the settings POST no longer sends a threshold',
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

// -- V23: the answer limit can be raised whichever backend is in use ----------
// The warning for a summary cut short says to raise the maximum answer length,
// and a remote server is sent it too -- but the field sat inside the embedded
// backend's own section, hidden the moment "Remote server" was picked.
{
  const at = html.indexOf('id="s_llmmaxtok"');
  const open = [];
  for (const m of html.slice(0, at).matchAll(/<(\/?)div\b([^>]*)>/g)) {
    if (m[1]) open.pop();
    else open.push((/\bid="([^"]+)"/.exec(m[2]) || [])[1] || '');
  }
  const inside = open.filter(id => id === 'advLlmEmbedded' || id === 'advLlmRemote');
  check('V23 the maximum answer length is not hidden with either backend',
        at > 0 && inside.length === 0, inside.join(', ') || (at > 0 ? '' : 'field not found'));
}

// -- the stylesheet's colour tokens -------------------------------------------
// The dark palette is the bare :root block; the light block redefines some of
// it and inherits the rest. Both checks below read colours the way the page
// resolves them, rather than trusting a literal that looks right in one theme.

function tokenBlock(re) {
  const m = css.match(re);
  const out = {};
  for (const t of ((m && m[1]) || '').matchAll(/(--[\w-]+)\s*:\s*(#[0-9a-fA-F]{6})\b/g)) {
    out[t[1]] = t[2].toLowerCase();
  }
  return out;
}
const darkTokens  = tokenBlock(/:root\s*\{([^}]*)\}/);
const lightTokens = Object.assign({}, darkTokens,
                                  tokenBlock(/:root\[data-theme="light"\]\s*\{([^}]*)\}/));

// WCAG relative luminance and contrast ratio.
function luminance(hex) {
  const [r, g, b] = [1, 3, 5].map(i => parseInt(hex.slice(i, i + 2), 16) / 255)
    .map(v => (v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4)));
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}
function contrast(a, b) {
  const [hi, lo] = [luminance(a), luminance(b)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
}
const THEMES = [['dark', darkTokens], ['light', lightTokens]];

// -- V27: a hovered button keeps its label ------------------------------------
// The hover fill was the dark theme's colour written in as a literal, which the
// light theme never overrode. There every plain button went near-black under
// the pointer, behind a label of almost exactly that colour: 1.0:1, so the
// label vanished.
{
  const rule = css.match(/(?:^|\n)button:hover:not\(:disabled\)\s*\{([^}]*)\}/);
  const fill = rule && /background\s*:\s*([^;]+);/.exec(rule[1]);
  const token = fill && /^var\((--[\w-]+)\)$/.exec(fill[1].trim());
  check('V27 the button hover fill comes from a theme token', Boolean(token),
        fill ? fill[1].trim() : 'rule not found');
  if (token) {
    for (const [name, t] of THEMES) {
      const bg = t[token[1]], ink = t['--ink'];
      const r = bg && ink ? contrast(bg, ink) : 0;
      check(`V27 a hovered button's label reads in the ${name} theme`, r >= 4.5,
            `${ink} on ${bg}: ${r.toFixed(2)}:1`);
    }
  }
}

// -- V28: the light theme's amber can be read as text --------------------------
// --vu is the text colour of the timestamps, the player clock, the recording
// timer, the Live switch and more. In the light theme it was #c58a00, which
// reached 2.5:1 on a button and 2.8:1 on a panel, so all of it was hard to
// read. 4.5:1 is what WCAG asks of text this size.
for (const [name, t] of THEMES) {
  for (const surface of ['--bg', '--bg-2', '--panel', '--panel-2', '--hover']) {
    const r = t['--vu'] && t[surface] ? contrast(t['--vu'], t[surface]) : 0;
    check(`V28 ${name} amber text reads on ${surface}`, r >= 4.5,
          `${t['--vu']} on ${t[surface]}: ${r.toFixed(2)}:1`);
  }
}

// -- V31: what a caution says a download costs ---------------------------------
// The speaker-separation caution promised "≈ 75 MB" -- the size of the
// embedding model it used to fetch -- long after that became CAM++ at 28 MB.
// The real sizes are the specs in src/util/models.cpp; the cautions quote
// them, so the two are checked here. Megabytes as human_size() counts them,
// and "≈" taken as within a tenth.
{
  const models = read('src/util/models.cpp');
  const bytes = fn => {
    const m = models.match(new RegExp('ModelSpec ' + fn + '\\(\\)[\\s\\S]*?([\\d\']+)ULL'));
    return m ? Number(m[1].replace(/'/g, '')) : NaN;
  };
  const MB = 1024 * 1024;
  const cases = [
    ['note.diarMissing', (bytes('segmentation_spec') + bytes('embedding_spec')) / MB],
    ['note.vadMissing', bytes('vad_spec') / MB],
  ];
  for (const [key, real] of cases) {
    for (const lang of ['en', 'tr']) {
      const text = (STR[key] || {})[lang] || '';
      const m = /≈ ([\d.,]+) MB/.exec(text);
      const said = m ? parseFloat(m[1].replace(',', '.')) : NaN;
      check(`V31 ${key} (${lang}) quotes the download's real size`,
            Math.abs(said - real) <= real / 10,
            `says ${m ? m[1] : 'nothing'} MB, the download is ${real.toFixed(2)} MB`);
    }
  }
}

// -- the tray's calls into the page --------------------------------------------
// The tray menu drives the page by name: src/app/shell.cpp evaluates
// window.trayAction('record' | 'pause') and binds trayReport and trayShow for
// the page to call. Both sides check before calling, so a rename on either one
// throws nothing anywhere -- the tray's menu just stops doing anything.
{
  const shell = read('src/app/shell.cpp');
  const app = read('web/app.js');
  const actions = [...shell.matchAll(/window\.trayAction\('(\w+)'\)/g)].map(m => m[1]);
  const bindings = [...shell.matchAll(/window\.bind\("(\w+)"/g)].map(m => m[1]);
  const handler = app.match(/window\.trayAction = async \(what\) => \{[\s\S]*?\n\};/);
  check('shell.cpp still sends the tray actions', actions.length > 0, actions.join(', '));
  check('shell.cpp still binds the calls the page makes', bindings.length > 0,
        bindings.join(', '));
  check('web/app.js still defines window.trayAction', handler !== null);
  for (const a of actions) {
    check(`window.trayAction tells '${a}' apart`,
          Boolean(handler && handler[0].includes(`what === '${a}'`)));
  }
  for (const b of bindings) {
    check(`web/app.js calls window.${b}`, app.includes(`window.${b}(`));
  }
  const e = STR && STR['toast.trayPressRecord'];
  check('the tray\'s "start it here" toast is translated in both languages',
        Boolean(e && e.en && e.tr));
}

// -- the tray setting's choices -----------------------------------------------
// The settings menu offers them, config.json and the settings POST each keep
// only the values they recognise, and the tray reads one back. A choice the
// C++ does not know is quietly saved as the default: the menu would show it
// picked and the tray would go on doing something else.
{
  const menu = html.match(/<select id="s_tray">([\s\S]*?)<\/select>/);
  const offered = menu ? [...menu[1].matchAll(/value="([^"]*)"/g)].map(m => m[1]) : [];
  const rule = /if \((\w+(?:\.\w+)?) != "(\w+)" && \1 != "(\w+)"\) \1 = "(\w+)";/;
  const kept = src => {
    const line = read(src).split('\n').find(l => rule.test(l) && /\btray\b/.test(l));
    const m = line && rule.exec(line);
    return m ? [m[2], m[3], m[4]].sort() : [];
  };
  const tray = read('src/app/tray.cpp');
  const parsed = ['icon', 'off'].filter(v => tray.includes(`setting == "${v}"`));
  check('Settings → System tray still offers its choices', offered.length > 0,
        offered.join(', '));
  for (const src of ['src/config.cpp', 'src/app/server.cpp']) {
    check(`${src} keeps exactly the choices the menu offers`,
          kept(src).join() === [...offered].sort().join(),
          `menu [${offered}] vs kept [${kept(src)}]`);
  }
  check('tray_mode() reads every non-default choice', parsed.length === 2,
        parsed.join(', '));
  if (menu && STR) {
    for (const m of menu[1].matchAll(/data-i18n="([^"]+)"/g)) {
      check(`${m[1]} is translated in both languages`,
            Boolean(STR[m[1]] && STR[m[1]].en && STR[m[1]].tr));
    }
  }
}

console.log(failures === 0 ? '\ntables: all checks passed'
                           : `\ntables: ${failures} check(s) FAILED`);
process.exit(failures === 0 ? 0 : 1);
