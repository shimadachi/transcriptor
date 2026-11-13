// The settings dialog. One tab per section, ordered general -> specific: the
// things most people touch come first, and every knob that only matters once
// something is wrong lives in the last tab, "Advanced".
//
// Nothing here is applied until Save: the form is a copy of the engine's
// settings, edited locally, and sent back in one request.
import {useEffect, useRef, useState} from 'react';

import {t} from '../i18n.js';
import {api, post} from '../lib/api.js';
import {pctText} from '../lib/format.js';
import {app, useApp} from '../lib/store.js';
import {CLTHR_BY_LANG} from '../lib/tables.js';
import {applyTheme, setLang} from '../state/prefs.js';
import {DL_KINDS, closeSettings, listenToDownload, pollDownload,
        setDownloadKind} from '../state/settings.js';
import {loadTemplates} from '../state/studio.js';
import {hasTray, setTrayMode, syncTray} from '../state/tray.js';
import {ask, toast} from '../state/ui.js';
import {checkUpdates, hideUpdateBanner, versionInfo} from '../state/updates.js';
import {Select} from './Select.jsx';

const SET_TABS = [
  ['general', 'set.tabGeneral'], ['output', 'set.tabOutput'], ['llm', 'set.tabLlm'],
  ['tpl', 'set.tabTpl'], ['adv', 'set.tabAdv'],
];

const NOTE_STYLE = {fontFamily: 'var(--mono)', fontSize: '11px', color: 'var(--muted)',
                    lineHeight: 1.6};

// Which form field holds each catalog's pick.
const DL_FIELD = {llm: 'llmdl', whisper: 'model', live: 'livemodel'};
const DL_SELECT = {llm: 's_llmdl', whisper: 's_model', live: 's_livemodel'};

// ---- the form, from the engine's settings ----
function formFrom(s, themePref, lang) {
  const catalog = s.llm_catalog || [];
  return {
    theme: s.ui_theme || themePref,
    tray: s.tray || 'minimize',
    uilang: s.ui_language || lang,
    device: s.device,
    // "" is what a fresh install lands on: nothing is chosen until the user
    // chooses.
    model: s.whisper_model || '',
    livemodel: s.live_whisper_model || '',
    language: s.language,
    whisperpath: s.whisper_model_path || '',
    sysgain: String(s.system_gain),
    micgain: String(s.mic_gain),
    diar: !!s.enable_diarization,
    updates: !!s.check_updates,
    keeplive: s.keep_live_transcript !== false,
    nspk: String(s.num_speakers),
    segmodel: s.diar_segmentation_model || '',
    embmodel: s.diar_embedding_model || '',
    outdir: s.output_dir,
    saveaudio: !!s.save_audio,
    savetx: !!s.save_transcript,
    savesum: !!s.save_summary,
    autotx: !!s.auto_transcribe,
    autosum: !!s.auto_summarize,
    vram: !!s.manage_vram,
    llmbackend: s.llm_backend || 'embedded',
    llmpath: s.llm_model_path || '',
    llmctx: String(s.llm_ctx),
    llmgpu: String(s.llm_gpu_layers),
    llmmaxtok: String(s.llm_max_tokens),
    llmthink: !!s.llm_thinking,
    llmurl: s.llm_base_url,
    llmtimeout: String(s.llm_timeout),
    sumlang: s.summary_language,
    llmmodel: s.llm_model,
    llmdl: catalog.length ? catalog[0].id : '',
  };
}

// ---- the template editor ----
// The built-in prompts from the engine, the user's edits to them, and the
// templates the user wrote from scratch (id -> {label, prompt, context}). Both
// kinds are edited through the same three fields.
const isCustom = (T, key) => Object.prototype.hasOwnProperty.call(T.custom, key);

function tplFields(T, key) {
  if (isCustom(T, key)) {
    const c = T.custom[key];
    return {name: c.label || '', prompt: c.prompt || '', ctx: c.context || ''};
  }
  const def = T.defaults[key] || '';
  const ov = T.overrides[key] || {};
  return {name: '', prompt: (ov.prompt && ov.prompt.trim()) ? ov.prompt : def,
          ctx: ov.context || ''};
}

// Folds the fields on screen back into the template they belong to.
function tplStash(T) {
  const key = T.cur;
  if (!key) return T;
  const ctx = T.ctx.trim();
  if (isCustom(T, key)) {
    return {...T, custom: {...T.custom, [key]: {
      label: T.name.trim() || t('set.tplUntitled'),
      prompt: T.prompt.trim(), context: ctx,
    }}};
  }
  // Only store a prompt override when it actually differs from the default.
  const def = (T.defaults[key] || '').trim();
  const promptOv = (T.prompt.trim() && T.prompt.trim() !== def) ? T.prompt.trim() : '';
  const overrides = {...T.overrides};
  if (promptOv || ctx) overrides[key] = {prompt: promptOv, context: ctx};
  else delete overrides[key];
  return {...T, overrides};
}

function tplFrom(s) {
  const T = {
    defaults: s.template_defaults || {},
    overrides: JSON.parse(JSON.stringify(s.template_overrides || {})),
    custom: JSON.parse(JSON.stringify(s.custom_templates || {})),
    builtin: (s.templates || []).filter(x => !x.custom),
    order: (s.templates || []).map(x => x.value),
    cur: null, name: '', prompt: '', ctx: '',
  };
  const known = (s.summary_template in T.defaults) || isCustom(T, s.summary_template);
  T.cur = known ? s.summary_template : (T.order[0] || 'meeting');
  return {...T, ...tplFields(T, T.cur)};
}

// ---- small pieces of the form ----
function Field({id, label, children}) {
  return (
    <div className="field">
      <label htmlFor={id}>{label}</label>
      {children}
    </div>
  );
}

// A field whose control is the themed dropdown: the label names the button
// that stands in for the native select.
function SelectField({id, label, children, ...select}) {
  return (
    <div className="field">
      <label id={id + '_lbl'} htmlFor={id + '_btn'}>{label}</label>
      {children ? children(<Select id={id} labelled {...select}/>)
                : <Select id={id} labelled {...select}/>}
    </div>
  );
}

function Check({id, label, checked, onChange, disabled}) {
  return (
    <div className="field check">
      <input type="checkbox" id={id} checked={checked} disabled={disabled}
             onChange={e => onChange(e.target.checked)}/>
      <label htmlFor={id}>{label}</label>
    </div>
  );
}

function SettingsForm({focus}) {
  const st = useApp();
  const [data, setData] = useState(null);       // the engine's settings
  const [f, setF] = useState(null);             // the form
  const [T, setT] = useState(null);             // the template editor
  const [tab, setTab] = useState('general');
  const [remoteModels, setRemoteModels] = useState(null);
  // Per catalog: the note a download wrote over the default one, and whether
  // its Download button has turned into Cancel.
  const [notes, setNotes] = useState({});
  const [active, setActive] = useState({});
  const [cancelling, setCancelling] = useState({});
  const body = useRef(null);
  const tplName = useRef(null);

  const set = patch => setF(prev => ({...prev, ...patch}));
  const setNote = (kind, text, err) =>
    setNotes(prev => ({...prev, [kind]: text == null ? null : {text, err: !!err}}));
  const setDlActive = (kind, on) => {
    setActive(prev => ({...prev, [kind]: on}));
    if (on) setCancelling(prev => ({...prev, [kind]: false}));
  };

  useEffect(() => {
    let gone = false;
    (async () => {
      let s;
      try { s = await api('/api/settings'); } catch { closeSettings(); return; }
      if (gone) return;
      const now = app.get();
      setData(s);
      setF(formFrom(s, now.themePref, now.lang));
      setT(tplFrom(s));
      // A download that is already running belongs to whichever section
      // started it, so the panel picks its controls back up where they were.
      if (s.model_download && s.model_download.active &&
          DL_KINDS.includes(s.model_download.kind)) {
        setDownloadKind(s.model_download.kind);
        setDlActive(s.model_download.kind, true);
        pollDownload();
      }
    })();
    return () => { gone = true; };
  }, []);

  // Land on the control, not just the tab it lives in.
  useEffect(() => {
    if (!data || !focus) return;
    const el = document.getElementById(focus + '_btn');
    if (el && el.scrollIntoView) el.scrollIntoView({block: 'center'});
  }, [data, focus]);

  // What the download watcher reports, while this dialog is here to show it.
  useEffect(() => {
    listenToDownload(({kind, phase, d, s}) => {
      if (phase === 'active') {
        const pct = d.progress == null ? '' : pctText(d.progress);
        setNote(kind, (d.message || t('llm.downloading')) + pct);
        setDlActive(kind, true);
        return;
      }
      setDlActive(kind, false);
      if (phase === 'error') {
        // A download the user stopped is not a failure: same note, not in red.
        setNote(kind, d.error, !d.cancelled);
        return;
      }
      // Back to the default note, which now says the model is here.
      setNote(kind, null);
      if (!s) return;
      // The engine already pointed the settings at the new file; mirror that
      // here so saving the panel does not undo it. Both speech pickers list
      // the same files, so a download for either changes what the other marks
      // as on disk -- but only the one it was for is moved to it.
      setData(s);
      if (kind === 'whisper') {
        set({model: s.whisper_model || '', whisperpath: s.whisper_model_path || ''});
      } else if (kind === 'live') {
        set({livemodel: s.live_whisper_model || ''});
      } else {
        set({llmpath: s.llm_model_path || ''});
      }
    });
    return () => listenToDownload(null);
  }, []);

  if (!data || !f || !T) {
    return (
      <div className="modal set">
        <h2>{t('set.title')}</h2>
        <div className="modal-body"><span className="empty">{t('lib.loading')}</span></div>
        <div className="modal-actions">
          <button className="ghost" onClick={closeSettings}>{t('set.cancel')}</button>
        </div>
      </div>
    );
  }

  const showTab = name => {
    setTab(name);
    // The body is the scroller -- a tall tab must not leave the next one
    // scrolled halfway down.
    if (body.current) body.current.scrollTop = 0;
  };

  // ---- catalogs ----
  const catalogs = {llm: data.llm_catalog || [], whisper: data.whisper_catalog || [],
                    live: data.whisper_catalog || []};
  const stored = {whisper: data.whisper_model, live: data.live_whisper_model};
  const entry = (kind, id) => catalogs[kind].find(x => x.id === id);

  // The placeholder heads the speech lists with an unselected entry, so "no
  // model yet" is a state the dropdown can actually show. A stored name the
  // catalog does not list (a legacy model, or one put there by hand) is added
  // as an option of its own: a dropdown that cannot show its own saved value
  // reports "" for it, and the next save would write that "" back.
  const catalogOptions = kind => {
    const out = [];
    if (kind !== 'llm') out.push({value: '', label: t('stt.none')});
    catalogs[kind].forEach(m => out.push({
      value: m.id,
      label: m.label + ' · ' + m.size + (m.downloaded ? t('llm.downloaded') : ''),
    }));
    if (stored[kind] && !entry(kind, stored[kind])) {
      out.push({value: stored[kind], label: stored[kind] + t('stt.notListed')});
    }
    return out;
  };

  // Default note for the highlighted catalog entry: what it is, then whether
  // it is here or what fetching it will cost.
  const defaultNote = kind => {
    const m = entry(kind, f[DL_FIELD[kind]]);
    // The catalog's own notes are written for the finished transcript, where
    // the biggest model is the recommendation. For the live one, what matters
    // is whether it keeps up, so that is what the note says instead.
    if (kind === 'live') {
      return (m ? (m.downloaded ? t('live.here') : t('live.willDl', {size: m.size})) : '') +
             t('live.pick');
    }
    if (!m) return kind === 'whisper' ? t('stt.pick') : '';
    return m.downloaded ? (m.note + t('llm.already')) : (m.note + t('llm.willDl') + m.size);
  };
  const note = kind => notes[kind] || {text: defaultNote(kind), err: false};
  const pickCatalog = (kind, id) => { set({[DL_FIELD[kind]]: id}); setNote(kind, null); };

  const startDownload = async kind => {
    const id = f[DL_FIELD[kind]];
    const m = entry(kind, id);
    if (!m) return;
    if (m.downloaded) {
      // Nothing to fetch -- just select it, which is what the user meant.
      if (kind === 'whisper') {
        set({whisperpath: ''});   // the picked model, not a stale path
      } else if (kind === 'llm') {
        set({llmpath: m.path});
        try { setData(await api('/api/settings')); } catch { /* the list stays */ }
      }
      toast(t('toast.dlAlready'));
      return;
    }
    const go = await ask({
      title: t('ask.downloadTitle'),
      body:  t('llm.confirmDl', {label: m.label, size: m.size}),
      yes:   t('set.llmDlBtn'),
    });
    if (!go) return;

    setDownloadKind(kind);
    setDlActive(kind, true);
    setNote(kind, t('llm.starting'));
    const r = await post('/api/model/download', {kind, id});
    if (r.error) {
      setDlActive(kind, false);   // both controls back, not just Download
      setNote(kind, r.error, true);
      return;
    }
    pollDownload();
  };

  const cancelDownload = async kind => {
    // Ask, then let the watcher tell the story: the download thread kills its
    // fetch, drops the partial file and goes inactive, which resets the buttons.
    setCancelling(prev => ({...prev, [kind]: true}));
    setNote(kind, t('llm.cancelling'));
    try { await post('/api/model/download/cancel'); } catch { /* the watcher recovers */ }
  };

  // Download and Cancel are the same control in two states: only one of them
  // is ever useful, so only one is ever on screen.
  const dlButtons = (kind, ids, cancelKeys) => (
    <>
      <button id={ids[0]} className="mini" type="button" disabled={!!active[kind]}
              onClick={() => startDownload(kind)}>{t(kind === 'llm' ? 'set.llmDlBtn'
                                                                    : 'set.whisperDlBtn')}</button>
      {active[kind] && (
        <button id={ids[1]} className="mini danger" type="button" title={t(cancelKeys[0])}
                disabled={!!cancelling[kind]}
                onClick={() => cancelDownload(kind)}>{t(cancelKeys[1])}</button>
      )}
    </>
  );
  const dlNote = (kind, id) => (
    <div id={id} className={'dlnote' + (note(kind).err ? ' err' : '')}>{note(kind).text}</div>
  );

  // ---- devices ----
  // The GPUs the engine reports, named the way the badge names them. Read
  // fresh every time Settings opens: an eGPU can come and go between visits.
  const devices = data.devices || [];
  const deviceOptions = [{value: 'auto', label: t('set.auto')}]
    .concat(devices.map(d => ({
      value: d.id,
      label: d.name + ' (' + d.backend + ')' +
             (d.vram ? ' · ' + (d.vram / 1073741824).toFixed(1) + ' GB' : '') +
             (d.integrated ? ' · ' + t('set.integrated') : ''),
    })), [{value: 'cpu', label: 'CPU'}]);
  // A card named in the settings but missing now: keep it visible rather than
  // silently snapping to Automatic, so it is clear what the app is doing.
  if (data.device && data.device !== 'auto' && data.device !== 'cpu' &&
      !devices.some(d => d.id === data.device)) {
    deviceOptions.push({value: data.device, label: data.device + ' · ' + t('set.deviceGone')});
  }

  // ---- summarizer models ----
  const ggufs = data.gguf_models || [];
  const ggufOptions = [{value: '', label: t(ggufs.length ? 'llm.manualPath' : 'llm.noModel')}]
    .concat(ggufs.map(p => ({value: p, label: p.split(/[\\/]/).pop()})));
  const rescanGguf = async () => {
    const s = await api('/api/settings');
    setData(prev => ({...prev, gguf_models: s.gguf_models}));
    toast((s.gguf_models || []).length + t('toast.modelsFound'));
  };

  const remoteOptions = remoteModels
    ? remoteModels.map(m => ({value: m, label: m}))
    : [{value: data.llm_model, label: data.llm_model || t('llm.autoFirst')}];
  const fetchModels = async () => {
    // Send the backend along with the URL. Without it the engine fell back to
    // the saved one, so picking "Remote server" and pressing Fetch before
    // saving listed the .gguf files on disk and never contacted the server.
    const d = await post('/api/llm/models', {llm_backend: f.llmbackend,
                                             llm_base_url: f.llmurl});
    if (d.error || !d.models.length) { toast(d.error || t('toast.noModel')); return; }
    setRemoteModels(d.models);
    set({llmmodel: d.models[0]});
  };

  // ---- templates ----
  const custom = isCustom(T, T.cur);
  const builtinLabel = Object.fromEntries(T.builtin.map(x => [x.value, x.label]));
  const tplOptions = T.order.map(id => ({
    value: id,
    // Keep the label in step with the name as it is typed.
    label: isCustom(T, id)
      ? (id === T.cur ? (T.name.trim() || t('set.tplUntitled')) : T.custom[id].label)
      : builtinLabel[id],
  }));
  const pickTpl = id => setT(prev => {
    const next = tplStash(prev);
    return {...next, cur: id, ...tplFields(next, id)};
  });
  // New template: seeded from whatever is on screen, so it starts from a
  // working prompt instead of a blank box. The id is minted once and never
  // changes, so renaming it later cannot orphan a saved summary_template.
  const newTpl = () => {
    const id = 'custom-' + Date.now();
    setT(prev => {
      const next = tplStash(prev);
      const made = {label: t('set.tplNewName'),
                    prompt: prev.prompt.trim() || prev.defaults[prev.cur] || '', context: ''};
      const withIt = {...next, custom: {...next.custom, [id]: made}, order: next.order.concat(id)};
      return {...withIt, cur: id, ...tplFields(withIt, id)};
    });
    setTimeout(() => {
      if (tplName.current) { tplName.current.focus(); tplName.current.select(); }
    }, 0);
  };
  const deleteTpl = () => setT(prev => {
    if (!isCustom(prev, prev.cur)) return prev;
    const customs = {...prev.custom};
    delete customs[prev.cur];
    const order = prev.order.filter(id => id !== prev.cur);
    const next = {...prev, custom: customs, order, cur: order[0] || 'meeting'};
    return {...next, ...tplFields(next, next.cur)};
  });

  // ---- save ----
  const save = async () => {
    const stashed = tplStash(T);   // capture edits for the currently-open template
    setT(stashed);

    // A custom template with no prompt has nothing to instruct the summarizer
    // with; the engine drops those, so say so rather than losing them silently.
    const customs = {};
    let dropped = 0;
    Object.entries(stashed.custom).forEach(([id, c]) => {
      if (c.prompt && c.prompt.trim()) customs[id] = c; else dropped++;
    });

    const r = await post('/api/settings', {
      template_overrides: stashed.overrides,
      custom_templates: customs,
      device: f.device, whisper_model: f.model,
      live_whisper_model: f.livemodel,
      keep_live_transcript: f.keeplive,
      language: f.language,
      whisper_model_path: f.whisperpath,

      enable_diarization: f.diar,
      num_speakers: parseInt(f.nspk, 10) || 0,
      diar_segmentation_model: f.segmodel,
      diar_embedding_model: f.embmodel,

      output_dir: f.outdir, save_audio: f.saveaudio,
      save_transcript: f.savetx, save_summary: f.savesum,
      auto_transcribe: f.autotx,
      auto_summarize: f.autosum, manage_vram: f.vram,
      check_updates: f.updates,

      llm_backend: f.llmbackend,
      llm_model_path: f.llmpath,
      llm_ctx: parseInt(f.llmctx, 10),
      llm_gpu_layers: parseInt(f.llmgpu, 10),
      llm_max_tokens: parseInt(f.llmmaxtok, 10),
      llm_thinking: f.llmthink,
      llm_base_url: f.llmurl, llm_model: f.llmmodel,
      llm_timeout: parseFloat(f.llmtimeout),
      summary_language: f.sumlang,
      ui_language: f.uilang,
      ui_theme: f.theme,
      tray: f.tray,
      system_gain: parseFloat(f.sysgain),
      mic_gain: parseFloat(f.micgain),
    });
    // Nothing was persisted if the engine said no -- it refuses while a job
    // runs. Closing the dialog and reporting "Settings saved" regardless threw
    // away every edit in it, template prompts included, with nothing on screen
    // to say so.
    if (r.error) { toast(r.error); return; }

    // Already persisted by the POST above, so only apply them locally.
    if (f.uilang !== st.lang) setLang(f.uilang, false);
    if (f.theme !== st.themePref) applyTheme(f.theme, false);
    // The tray follows the new choice, and the new language, straight away.
    setTrayMode(f.tray);
    syncTray(true);
    // Switching the check off takes the banner down now rather than at the
    // next launch; switching it on looks straight away, so the setting visibly
    // does something either way.
    if (f.updates) {
      checkUpdates({check_updates: true, version: data.version || versionInfo().version,
                    repo: data.repo || versionInfo().repo});
    } else {
      hideUpdateBanner();
    }
    closeSettings();
    if (r.device && app.get().s) app.set(prev => ({s: {...prev.s, device: r.device}}));
    loadTemplates();  // picks up new/renamed templates and any label language change
    toast(dropped ? t('toast.savedDropped', {n: dropped}) : t('toast.saved'));
  };

  const embedded = f.llmbackend !== 'remote';
  const clthr = CLTHR_BY_LANG[f.language];
  const text = (key, type = 'text', extra = {}) => ({
    type, value: f[key], onChange: e => set({[key]: e.target.value}), ...extra,
  });
  const langOptions = [{value: 'en', label: 'English'}, {value: 'tr', label: 'Türkçe'}];

  return (
    <div className="modal set">
      <h2>{t('set.title')}</h2>

      <nav className="tabs set-tabs" role="tablist">
        {SET_TABS.map(([name, key]) => (
          <button key={name} id={'setTab_' + name} type="button" role="tab"
                  className={'tab' + (tab === name ? ' on' : '')}
                  aria-selected={tab === name} aria-controls={'setView_' + name}
                  onClick={() => showTab(name)}>{t(key)}</button>
        ))}
      </nav>

      {/* Only this scrolls. The title, the tabs and the Cancel/Save row stay
          put, so the dialog is the same shape whichever tab is open and the
          buttons are never below the fold. */}
      <div className="modal-body" id="setBody" ref={body}>

        <div id="setView_general" className="set-view" role="tabpanel"
             aria-labelledby="setTab_general" hidden={tab !== 'general'}>
          <div className="grp">
            <div className="t">{t('set.generalGrp')}</div>
            <SelectField id="s_uilang" label={t('set.uiLang')} value={f.uilang}
                         options={langOptions} onChange={v => set({uilang: v})}/>
            <SelectField id="s_theme" label={t('set.theme')} value={f.theme}
                         onChange={v => set({theme: v})}
                         options={[{value: 'system', label: t('set.themeSystem')},
                                   {value: 'light', label: t('set.themeLight')},
                                   {value: 'dark', label: t('set.themeDark')}]}/>
            {/* Only where there is a tray to put an icon in: Windows and Linux. */}
            {hasTray && (
              <SelectField id="s_tray" label={t('set.tray')} value={f.tray}
                           onChange={v => set({tray: v})}
                           options={[{value: 'minimize', label: t('set.trayMinimize')},
                                     {value: 'icon', label: t('set.trayIcon')},
                                     {value: 'off', label: t('set.trayOff')}]}/>
            )}
            <SelectField id="s_language" label={t('set.lang')} value={f.language}
                         onChange={v => set({language: v})}
                         options={[{value: '', label: t('set.autoDetect')}].concat(langOptions)}/>
            <SelectField id="s_sumlang" label={t('set.sumLang')} value={f.sumlang}
                         options={langOptions} onChange={v => set({sumlang: v})}/>
            <SelectField id="s_model" label={t('set.whisper')} grow value={f.model}
                         options={catalogOptions('whisper')}
                         onChange={v => pickCatalog('whisper', v)}>
              {select => (<>
                <div className="row">{select}
                  {dlButtons('whisper', ['dlWhisper', 'cancelWhisperDl'],
                             ['set.dlCancelTitle', 'set.dlCancel'])}</div>
                {dlNote('whisper', 'whisperDlNote')}
              </>)}
            </SelectField>
            {/* Chosen apart from the model above: the live transcript has to
                keep up with someone talking, and the model the finished
                transcript is made with rarely can. The switch itself is in
                the studio. */}
            <SelectField id="s_livemodel" label={t('set.liveModel')} grow value={f.livemodel}
                         options={catalogOptions('live')}
                         onChange={v => pickCatalog('live', v)}>
              {select => (<>
                <div className="row">{select}
                  {dlButtons('live', ['dlLive', 'cancelLiveDl'],
                             ['set.dlCancelTitle', 'set.dlCancel'])}</div>
                {dlNote('live', 'liveDlNote')}
              </>)}
            </SelectField>
            {/* On by default: a take Live heard from start to finish needs no
                second pass. One it did not is never kept as though it were. */}
            <Check id="s_keeplive" label={t('set.keepLive')} checked={f.keeplive}
                   onChange={v => set({keeplive: v})}/>
            <SelectField id="s_device" label={t('set.device')} value={f.device}
                         options={deviceOptions} onChange={v => set({device: v})}>
              {select => (<>
                {select}
                <div style={{...NOTE_STYLE, marginTop: '6px'}}>{t('set.deviceNote')}</div>
              </>)}
            </SelectField>
            <Check id="s_diar" label={t('set.diarOn')} checked={f.diar}
                   disabled={!data.diar_supported} onChange={v => set({diar: v})}/>
            <Check id="s_updates" label={t('set.updates')} checked={f.updates}
                   onChange={v => set({updates: v})}/>
            <div className="field" style={NOTE_STYLE}>
              <span>{t('set.version')}</span> <code id="s_version">{data.version || ''}</code>
            </div>
          </div>
        </div>

        <div id="setView_output" className="set-view" role="tabpanel"
             aria-labelledby="setTab_output" hidden={tab !== 'output'}>
          <div className="grp">
            <div className="t">{t('set.outGrp')}</div>
            <Field id="s_outdir" label={t('set.outDir')}>
              <input id="s_outdir" placeholder="~/Transcriptor" {...text('outdir')}/></Field>
            <Check id="s_saveaudio" label={t('set.saveAudio')} checked={f.saveaudio}
                   onChange={v => set({saveaudio: v})}/>
            <Check id="s_savetx" label={t('set.saveTx')} checked={f.savetx}
                   onChange={v => set({savetx: v})}/>
            <Check id="s_savesum" label={t('set.saveSum')} checked={f.savesum}
                   onChange={v => set({savesum: v})}/>
            <Check id="s_autotx" label={t('set.autoTx')} checked={f.autotx}
                   onChange={v => set({autotx: v})}/>
            <Check id="s_autosum" label={t('set.autoSum')} checked={f.autosum}
                   onChange={v => set({autosum: v})}/>
            <div className="field" style={NOTE_STYLE}>{t('set.outNote')}</div>
          </div>
        </div>

        <div id="setView_llm" className="set-view" role="tabpanel"
             aria-labelledby="setTab_llm" hidden={tab !== 'llm'}>
          <div className="grp">
            <div className="t">{t('set.llmGrp')}</div>
            <SelectField id="s_llmbackend" label={t('set.llmMode')} value={f.llmbackend}
                         onChange={v => set({llmbackend: v})}
                         options={[{value: 'embedded', label: t('set.llmEmbedded')},
                                   {value: 'remote', label: t('set.llmRemote')}]}/>

            {/* Only the fields that belong to the selected backend. */}
            <div id="llmEmbedded" style={{display: embedded ? '' : 'none'}}>
              <SelectField id="s_llmdl" label={t('set.llmDl')} grow value={f.llmdl}
                           options={catalogOptions('llm')}
                           onChange={v => pickCatalog('llm', v)}>
                {select => (<>
                  <div className="row">{select}
                    {dlButtons('llm', ['dlLlm', 'cancelLlmDl'],
                               ['set.llmDlCancelTitle', 'set.llmDlCancel'])}</div>
                  {dlNote('llm', 'llmDlNote')}
                </>)}
              </SelectField>
              {/* Picking from the list fills the path box under Advanced,
                  which is what actually gets saved. */}
              <SelectField id="s_ggufsel" label={t('set.gguf')} grow
                           value={ggufs.includes(f.llmpath) ? f.llmpath : ''}
                           options={ggufOptions}
                           onChange={v => { if (v) set({llmpath: v}); }}>
                {select => (
                  <div className="row">{select}
                    <button id="rescanGguf" className="mini" type="button"
                            onClick={rescanGguf}>{t('set.scan')}</button></div>
                )}
              </SelectField>
            </div>

            <div id="llmRemote" style={{display: embedded ? 'none' : ''}}>
              <Field id="s_llmurl" label={t('set.llmUrl')}>
                <input id="s_llmurl" placeholder="http://127.0.0.1:1234/v1" {...text('llmurl')}/>
              </Field>
              <SelectField id="s_llmmodel" label={t('set.model')} grow value={f.llmmodel}
                           options={remoteOptions} onChange={v => set({llmmodel: v})}>
                {select => (
                  <div className="row">{select}
                    <button id="fetchModels" className="mini" type="button"
                            onClick={fetchModels}>{t('set.fetch')}</button></div>
                )}
              </SelectField>
            </div>
          </div>
        </div>

        <div id="setView_tpl" className="set-view" role="tabpanel"
             aria-labelledby="setTab_tpl" hidden={tab !== 'tpl'}>
          <div className="grp">
            <div className="t">{t('set.tplGrp')}</div>
            <SelectField id="s_tplsel" label={t('set.tplPick')} value={T.cur}
                         options={tplOptions} onChange={pickTpl}/>
            {/* Custom templates get a name field and a delete button;
                built-ins get a reset. */}
            {custom && (
              <Field id="s_tplname" label={t('set.tplName')}>
                <input ref={tplName} type="text" id="s_tplname" maxLength={60}
                       placeholder={t('set.tplNamePh')} value={T.name}
                       onChange={e => setT(prev => ({...prev, name: e.target.value}))}/>
              </Field>
            )}
            <Field id="s_tplprompt" label={t('set.tplPrompt')}>
              <textarea id="s_tplprompt" rows="6" value={T.prompt}
                        onChange={e => setT(prev => ({...prev, prompt: e.target.value}))}></textarea>
            </Field>
            <Field id="s_tplctx" label={t('set.tplCtx')}>
              <textarea id="s_tplctx" rows="3" placeholder={t('set.tplCtxPh')} value={T.ctx}
                        onChange={e => setT(prev => ({...prev, ctx: e.target.value}))}></textarea>
            </Field>
            <div className="field tpl-actions">
              <button id="s_tplnew" className="mini" type="button"
                      onClick={newTpl}>{t('set.tplNew')}</button>
              {!custom && (
                <button id="s_tplreset" className="mini" type="button"
                        onClick={() => setT(prev => ({...prev, prompt: prev.defaults[prev.cur] || ''}))}>
                  {t('set.tplReset')}</button>
              )}
              {custom && (
                <button id="s_tpldel" className="mini danger" type="button"
                        onClick={deleteTpl}>{t('set.tplDel')}</button>
              )}
            </div>
          </div>
        </div>

        <div id="setView_adv" className="set-view" role="tabpanel"
             aria-labelledby="setTab_adv" hidden={tab !== 'adv'}>
          <div className="adv-hint">{t('set.advHint')}</div>

          <div className="grp">
            <div className="t">{t('set.sttGrp')}</div>
            <Field id="s_whisperpath" label={t('set.modelFile')}>
              <input id="s_whisperpath" placeholder="…/ggml-large-v3.bin" {...text('whisperpath')}/>
            </Field>
          </div>

          <div className="grp">
            <div className="t">{t('set.mixGrp')}</div>
            <Field id="s_sysgain" label={t('set.sysGain')}>
              <input id="s_sysgain" min="0" max="4" step="0.1" {...text('sysgain', 'number')}/>
            </Field>
            <Field id="s_micgain" label={t('set.micGain')}>
              <input id="s_micgain" min="0" max="4" step="0.1" {...text('micgain', 'number')}/>
            </Field>
            <div className="field" style={NOTE_STYLE}>{t('set.mixNote')}</div>
          </div>

          <div className="grp">
            <div className="t">{t('set.diarGrp')}</div>
            <Field id="s_nspk" label={t('set.nspk')}>
              <input id="s_nspk" min="0" max="20" step="1" {...text('nspk', 'number')}/>
            </Field>
            {/* Not a setting: derived from the transcription language by the
                engine. The panel still says which value that is, and follows
                the language select above as it is changed. */}
            <div className="field"><div id="clthrNote" className="dlnote">
              {t('set.clthr').replace(
                '{v}', (clthr === undefined ? CLTHR_BY_LANG[''] : clthr).toFixed(2))}
            </div></div>
            <Field id="s_segmodel" label={t('set.segModel')}>
              <input id="s_segmodel" placeholder="…/segmentation.onnx" {...text('segmodel')}/>
            </Field>
            <Field id="s_embmodel" label={t('set.embModel')}>
              <input id="s_embmodel" placeholder="…/speaker-embedding-campplus.onnx"
                     {...text('embmodel')}/>
            </Field>
            <div className="field" style={NOTE_STYLE}>
              <span>{t('set.diarNote1')}</span>{' '}
              <code id="s_modelsdir">{data.models_dir || ''}</code>
              <span>{t('set.diarNote2')}</span>
            </div>
          </div>

          <div className="grp">
            <div className="t">{t('set.llmTuneGrp')}</div>
            <div id="advLlmEmbedded" style={{display: embedded ? '' : 'none'}}>
              <Field id="s_llmpath" label={t('set.llmPath')}>
                <input id="s_llmpath" placeholder="…/qwen2.5-7b-instruct-q4_k_m.gguf"
                       {...text('llmpath')}/>
              </Field>
              <Field id="s_llmctx" label={t('set.llmCtx')}>
                <input id="s_llmctx" min="0" max="131072" step="1024"
                       {...text('llmctx', 'number')}/>
              </Field>
              <Field id="s_llmgpu" label={t('set.llmGpu')}>
                <input id="s_llmgpu" min="0" max="999" step="1" {...text('llmgpu', 'number')}/>
              </Field>
              <div className="field" style={NOTE_STYLE}>
                <span>{t('set.llmNote1')}</span><code>.gguf</code>
                <span>{t('set.llmNote2')}</span><code id="s_modelsdir2">{data.models_dir || ''}</code>
                <span>{t('set.llmNote3')}</span>
              </div>
            </div>
            <div id="advLlmRemote" style={{display: embedded ? 'none' : ''}}>
              <Field id="s_llmtimeout" label={t('set.llmTimeout')}>
                <input id="s_llmtimeout" min="10" max="3600" step="10"
                       {...text('llmtimeout', 'number')}/>
              </Field>
            </div>
            {/* Both backends: a remote server is sent it as max_tokens, and the
                warning for a summary cut short points here whichever is used. */}
            <Field id="s_llmmaxtok" label={t('set.llmMaxTok')}>
              <input id="s_llmmaxtok" min="64" max="16384" step="64"
                     {...text('llmmaxtok', 'number')}/>
            </Field>
            <Check id="s_llmthink" label={t('set.llmThink')} checked={f.llmthink}
                   onChange={v => set({llmthink: v})}/>
            <Check id="s_vram" label={t('set.vram')} checked={f.vram}
                   onChange={v => set({vram: v})}/>
          </div>
        </div>

      </div>

      <div className="modal-actions">
        <button id="cancelSettings" className="ghost" onClick={closeSettings}>{t('set.cancel')}</button>
        <button id="saveSettings" className="save" onClick={save}>{t('set.save')}</button>
      </div>
    </div>
  );
}

export function SettingsDialog() {
  const {settings} = useApp();
  return (
    <div className={'modal-bg' + (settings ? ' on' : '')} id="modalBg"
         onClick={e => { if (e.target === e.currentTarget) closeSettings(); }}>
      {settings && <SettingsForm focus={settings.focus}/>}
    </div>
  );
}
