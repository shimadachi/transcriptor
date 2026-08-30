// ---- UI language (en / tr) ----
// English is the source language: it is what sits inline in index.html, so the
// page reads correctly before this file runs, and it is what an unknown key
// falls back to. Every visible string has an entry here; applyLang() rewrites
// the DOM from it.
//
// Markup hooks:  data-i18n="key"        -> textContent
//                data-i18n-html="key"   -> innerHTML (strings with tags)
//                data-i18n-title="key"  -> title attribute
//                data-i18n-ph="key"     -> placeholder attribute

const STR = {
  // -- chrome ---------------------------------------------------------------
  'app.title':      {en: 'Transcriptor · audio · text · summary',
                     tr: 'Transcriptor · ses · metin · özet'},
  'hdr.settings':   {en: '⚙ Settings', tr: '⚙ Ayarlar'},
  'tab.studio':     {en: 'Studio', tr: 'Stüdyo'},
  'tab.library':    {en: 'Library', tr: 'Arşiv'},

  // -- console --------------------------------------------------------------
  'rec.start':      {en: 'Start recording', tr: 'Kaydı başlat'},
  'rec.recording':  {en: 'Recording', tr: 'Kayıtta'},
  'src.label':      {en: 'Source', tr: 'Kaynak'},
  'src.micMix':     {en: 'Mix a microphone into this source',
                     tr: 'Mikrofonu bu kaynağa karıştır'},
  'src.refresh':    {en: 'Refresh', tr: 'Yenile'},
  'src.file':       {en: '⇪ File', tr: '⇪ Dosya'},
  'src.fileTitle':  {en: 'Process a saved audio file', tr: 'Kayıtlı ses dosyası işle'},
  'src.pause':      {en: '⏸ Pause', tr: '⏸ Duraklat'},
  'src.resume':     {en: '▶ Resume', tr: '▶ Sürdür'},
  'src.pauseTitle': {en: 'Pause / resume', tr: 'Duraklat/Sürdür'},
  'src.cancel':     {en: '✕ Cancel', tr: '✕ İptal'},
  'src.cancelTitle':{en: 'Cancel and delete the recording / output',
                     tr: 'Kaydı/çıktıyı iptal et ve sil'},
  'src.stop':       {en: '■ Stop', tr: '■ Durdur'},
  'src.ready':      {en: 'Ready', tr: 'Hazır'},
  'src.browserBoth':  {en: '🎙 Browser — Microphone + System',
                       tr: '🎙 Tarayıcı — Mikrofon + Sistem'},
  'src.browserSys':   {en: '🖥 Browser — System / Tab audio',
                       tr: '🖥 Tarayıcı — Sistem / Sekme sesi'},
  'src.browserMic':   {en: '🎤 Browser — Microphone', tr: '🎤 Tarayıcı — Mikrofon'},
  'src.micNoneAvail': {en: '+ Mic: none', tr: '+ Mik: yok'},
  'src.micNone':      {en: '+ No mic', tr: '+ Mik yok'},
  'saved.k':        {en: 'Saved →', tr: 'Kaydedildi →'},
  'saved.open':     {en: 'Open Folder', tr: 'Klasörü Aç'},

  // -- status / notices -----------------------------------------------------
  'st.browserPaused': {en: 'Browser recording paused…',
                       tr: 'Tarayıcı kaydı duraklatıldı…'},
  'st.browserRec':    {en: 'Recording in the browser…',
                       tr: 'Tarayıcıda kayıt sürüyor…'},
  'st.pausedSuffix':  {en: ' · paused', tr: ' · duraklatıldı'},
  'note.sttMissing':  {en: '<b>The transcription model is not downloaded yet.</b> It downloads automatically on the first recording (large-v3 ≈ 3 GB).',
                       tr: '<b>Metin modeli henüz inmedi.</b> İlk kayıtta otomatik indirilecek (large-v3 ≈ 3 GB).'},
  'note.diarMissing': {en: '<b>The speaker-separation models are not downloaded yet.</b> They download automatically on the first recording (≈ 75 MB).',
                       tr: '<b>Konuşmacı ayrımı modelleri henüz inmedi.</b> İlk kayıtta otomatik indirilecek (≈ 75 MB).'},
  'note.diarUnbuilt': {en: '<b>This build was compiled without speaker separation.</b> The transcript comes out unlabelled.',
                       tr: '<b>Bu sürüm konuşmacı ayrımı olmadan derlendi.</b> Metin etiketsiz üretilir.'},

  // -- transcript / summary -------------------------------------------------
  'tx.label':       {en: 'Transcript', tr: 'Metin'},
  'tx.empty':       {en: 'Start and stop a recording — the transcript appears here.',
                     tr: 'Kaydı başlatıp durdurun — deşifre burada belirir.'},
  'tx.none':        {en: 'No transcript found.', tr: 'Metin bulunamadı.'},
  'sum.label':      {en: 'Summary', tr: 'Özet'},
  'sum.tplTitle':   {en: 'Note template', tr: 'Not şablonu'},
  'sum.ctx':        {en: '＋ Context', tr: '＋ Bağlam'},
  'sum.ctxTitle':   {en: 'Add meeting context', tr: 'Toplantı bağlamı ekle'},
  'sum.go':         {en: '✦ Summarize', tr: '✦ Özetle'},
  'sum.phTitle':    {en: 'Title (e.g. Q3 Budget Meeting)',
                     tr: 'Başlık (ör. Q3 Bütçe Toplantısı)'},
  'sum.phPeople':   {en: 'Participants (comma separated)',
                     tr: 'Katılımcılar (virgülle ayırın)'},
  'sum.phNotes':    {en: 'Extra context / agenda / special instructions — included in the summary',
                     tr: 'Ek bağlam / gündem / özel talimat — özete dahil edilir'},

  // -- library --------------------------------------------------------------
  'lib.label':      {en: 'Library', tr: 'Arşiv'},
  'lib.refresh':    {en: 'Rescan the output folder', tr: 'Çıktı klasörünü yeniden tara'},
  'lib.loading':    {en: 'Loading…', tr: 'Yükleniyor…'},
  'lib.pick':       {en: 'Pick a recording on the left.', tr: 'Soldan bir kayıt seçin.'},
  'lib.none':       {en: 'No saved recordings yet. They land in the output folder set in Settings.',
                     tr: 'Henüz kayıt yok. Kayıtlar, Ayarlar\'daki çıktı klasörüne düşer.'},
  'lib.noAudio':    {en: 'No audio was saved for this recording.',
                     tr: 'Bu kayıt için ses saklanmamış.'},
  'lib.noTx':       {en: 'No transcript was saved for this recording.',
                     tr: 'Bu kayıt için metin saklanmamış.'},
  'lib.noSum':      {en: 'No summary was saved for this recording.',
                     tr: 'Bu kayıt için özet saklanmamış.'},
  'lib.badges.tx':  {en: 'text', tr: 'metin'},
  'lib.badges.sum': {en: 'summary', tr: 'özet'},
  'lib.badges.aud': {en: 'audio', tr: 'ses'},
  'lib.loadErr':    {en: 'That recording could not be opened', tr: 'Kayıt açılamadı'},

  // -- toasts ---------------------------------------------------------------
  'toast.cancelled':    {en: 'Cancelled', tr: 'İptal edildi'},
  'toast.uploading':    {en: 'Uploading: ', tr: 'Yükleniyor: '},
  'toast.uploadErr':    {en: 'Upload failed', tr: 'Yükleme hatası'},
  'toast.folderErr':    {en: 'Could not open the folder: ', tr: 'Klasör açılamadı: '},
  'toast.dlFailed':     {en: 'Download failed', tr: 'İndirme başarısız'},
  'toast.dlDone':       {en: 'Model downloaded', tr: 'Model indirildi'},
  'toast.dlAlready':    {en: 'Model already downloaded — selected',
                         tr: 'Model zaten inik — seçildi'},
  'toast.modelsFound':  {en: ' model(s) found', tr: ' model bulundu'},
  'toast.noModel':      {en: 'No model found', tr: 'Model bulunamadı'},
  'toast.saved':        {en: 'Settings saved', tr: 'Ayarlar kaydedildi'},
  'toast.savedDropped': {en: 'Settings saved · skipped {n} template(s) with an empty prompt',
                         tr: 'Ayarlar kaydedildi · yönergesi boş {n} şablon atlandı'},
  'toast.noBrowserRec': {en: 'Browser recording is not supported (needs a secure context / localhost)',
                         tr: 'Tarayıcı kaydı desteklenmiyor (güvenli bağlam / localhost gerekir)'},
  'toast.noPermission': {en: 'Permission denied, or not supported',
                         tr: 'İzin verilmedi veya desteklenmiyor'},
  'toast.noAudio':      {en: 'No audio captured. On Linux only a TAB\'s audio can be shared (not a window/screen) — pick a tab and tick "share audio", or choose the system (loopback) source instead.',
                         tr: 'Ses alınamadı. Linux\'ta yalnızca bir SEKME sesi paylaşılabilir (pencere/ekran değil) — bir sekme seçip "sesi paylaş"ı işaretleyin, ya da kaynaktan sistem (loopback) seçin.'},
  'toast.recCancelled': {en: 'Recording cancelled', tr: 'Kayıt iptal edildi'},
  'toast.emptyRec':     {en: 'Empty recording', tr: 'Boş kayıt'},
  'toast.processing':   {en: 'Processing…', tr: 'İşleniyor…'},

  // -- settings: shared -----------------------------------------------------
  'set.title':      {en: 'Settings', tr: 'Ayarlar'},
  'set.cancel':     {en: 'Cancel', tr: 'İptal'},
  'set.save':       {en: 'Save', tr: 'Kaydet'},

  'set.generalGrp': {en: 'General', tr: 'Genel'},
  'set.uiLang':     {en: 'Interface language', tr: 'Arayüz dili'},
  'set.theme':      {en: 'Appearance', tr: 'Görünüm'},
  'set.themeSystem':{en: 'Follow the system', tr: 'Sistemi izle'},
  'set.themeLight': {en: 'Light', tr: 'Aydınlık'},
  'set.themeDark':  {en: 'Dark', tr: 'Karanlık'},
  'set.lang':       {en: 'Spoken language', tr: 'Konuşma dili'},
  'set.autoDetect': {en: 'Auto-detect', tr: 'Otomatik algıla'},
  'set.sumLang':    {en: 'Summary language', tr: 'Özet dili'},
  'set.whisper':    {en: 'Whisper model', tr: 'Whisper modeli'},
  'set.diarOn':     {en: 'Separate speakers', tr: 'Konuşmacıları ayır'},

  'set.outGrp':     {en: 'Output & Automation', tr: 'Çıktı & Otomasyon'},
  'set.outDir':     {en: 'Output folder', tr: 'Kayıt klasörü'},
  'set.saveAudio':  {en: 'Save the audio (audio.wav)', tr: 'Sesi kaydet (audio.wav)'},
  'set.saveTx':     {en: 'Save the transcript (transcript.txt/.json)',
                     tr: 'Metni kaydet (transcript.txt/.json)'},
  'set.saveSum':    {en: 'Save the summary (summary.txt)', tr: 'Özeti kaydet (summary.txt)'},
  'set.autoSum':    {en: 'Summarize automatically when processing finishes',
                     tr: 'İşleme bitince özeti otomatik çıkar'},
  'set.outNote':    {en: 'Every recording gets its own timestamped folder here. The Library tab reads this folder.',
                     tr: 'Her kayıt burada kendi tarihli klasörüne düşer. Arşiv sekmesi bu klasörü okur.'},

  'set.llmGrp':     {en: 'Summarizer · llama.cpp', tr: 'Özetleyici · llama.cpp'},
  'set.llmMode':    {en: 'Mode', tr: 'Çalışma biçimi'},
  'set.llmEmbedded':{en: 'Embedded (llama.cpp) — nothing to install',
                     tr: 'Gömülü (llama.cpp) — kurulum gerekmez'},
  'set.llmRemote':  {en: 'Remote server (LM Studio / Ollama / llama-server)',
                     tr: 'Uzak sunucu (LM Studio / Ollama / llama-server)'},
  'set.llmDl':      {en: 'Download a ready-made model (optional, one time)',
                     tr: 'Hazır model indir (isteğe bağlı, tek seferlik)'},
  'set.llmDlBtn':   {en: 'Download', tr: 'İndir'},
  'set.gguf':       {en: 'GGUF model', tr: 'GGUF modeli'},
  'set.scan':       {en: 'Scan', tr: 'Tara'},
  'set.llmUrl':     {en: 'Server URL', tr: 'Sunucu URL'},
  'set.model':      {en: 'Model', tr: 'Model'},
  'set.fetch':      {en: 'Fetch', tr: 'Getir'},

  'set.tplGrp':     {en: 'Note templates · edit', tr: 'Not şablonları · düzenle'},
  'set.tplPick':    {en: 'Template to edit', tr: 'Düzenlenecek şablon'},
  'set.tplName':    {en: 'Template name', tr: 'Şablon adı'},
  'set.tplNamePh':  {en: 'e.g. Customer Call', tr: 'Örn. Müşteri Görüşmesi'},
  'set.tplPrompt':  {en: 'Template instruction (the system prompt given to the summarizer)',
                     tr: 'Şablon yönergesi (özetleyiciye verilen sistem promptu)'},
  'set.tplCtx':     {en: 'Persistent extra context (added to every summary from this template)',
                     tr: 'Kalıcı ek bağlam (bu şablonun her özetine eklenir)'},
  'set.tplCtxPh':   {en: 'e.g. Our company is Acme; focus on decisions and action items.',
                     tr: 'Örn. Şirketimiz Acme; kararlara ve aksiyonlara odaklan.'},
  'set.tplNew':     {en: '+ New template', tr: '+ Yeni şablon'},
  'set.tplReset':   {en: 'Reset the instruction to default', tr: 'Yönergeyi varsayılana sıfırla'},
  'set.tplDel':     {en: 'Delete', tr: 'Sil'},
  'set.tplUntitled':{en: 'Template', tr: 'Şablon'},
  'set.tplNewName': {en: 'New template', tr: 'Yeni şablon'},

  // -- settings: advanced ---------------------------------------------------
  'set.advGrp':     {en: 'Advanced', tr: 'Gelişmiş'},
  'set.advHint':    {en: 'hardware, model files, fine-tuning',
                     tr: 'donanım, model dosyaları, ince ayar'},

  'set.sttGrp':     {en: 'Transcription · STT', tr: 'Metin · STT'},
  'set.device':     {en: 'Device', tr: 'Cihaz'},
  'set.auto':       {en: 'Automatic', tr: 'Otomatik'},
  'set.modelFile':  {en: 'Model file (empty = download automatically)',
                     tr: 'Model dosyası (boş = otomatik indir)'},

  'set.mixGrp':     {en: 'Audio mix · Microphone + System',
                     tr: 'Ses karışımı · Mikrofon + Sistem'},
  'set.sysGain':    {en: 'System audio gain (0–4)', tr: 'Sistem sesi kazancı (0–4)'},
  'set.micGain':    {en: 'Microphone gain (0–4)', tr: 'Mikrofon kazancı (0–4)'},
  'set.mixNote':    {en: 'Picking a microphone in the source row mixes it into the system audio. A peak limiter is applied automatically to prevent clipping.',
                     tr: 'Kaynak satırında bir mikrofon seçince sistem sesine karıştırılır. Kırpılmayı önlemek için tepe sınırlayıcı otomatik devrededir.'},

  'set.diarGrp':    {en: 'Speaker separation · sherpa-onnx',
                     tr: 'Konuşmacı ayrımı · sherpa-onnx'},
  'set.nspk':       {en: 'Number of speakers (0 = detect automatically)',
                     tr: 'Konuşmacı sayısı (0 = otomatik bul)'},
  'set.clthr':      {en: 'Clustering threshold (0.05–0.95) — a lower value yields more speakers',
                     tr: 'Ayrım eşiği (0.05–0.95) — düşük değer daha çok konuşmacı üretir'},
  'set.segModel':   {en: 'Segmentation model (empty = download automatically)',
                     tr: 'Bölütleme modeli (boş = otomatik indir)'},
  'set.embModel':   {en: 'Speaker-embedding model (empty = download automatically)',
                     tr: 'Ses izi modeli (boş = otomatik indir)'},
  'set.diarNote1':  {en: 'The ONNX build of pyannote segmentation-3.0 is used — no token needed, inference is fully local. The models download on first use into',
                     tr: 'pyannote segmentation-3.0 modelinin ONNX hâli kullanılır — token gerekmez, çıkarım tamamen yerel. Modeller ilk kullanımda'},
  // Leading space matters: this span sits straight after the <code> path with
  // no whitespace between them in the markup, and applyLang replaces the
  // element's text wholesale. English wants none — the sentence ends there.
  'set.diarNote2':  {en: '.', tr: ' klasörüne iner.'},

  'set.llmTuneGrp': {en: 'Summarizer · fine-tuning', tr: 'Özetleyici · ince ayar'},
  'set.llmPath':    {en: 'Model file path (type it if it is not in the list)',
                     tr: 'Model dosya yolu (listede yoksa elle yazın)'},
  'set.llmCtx':     {en: 'Context size (tokens)', tr: 'Bağlam boyutu (token)'},
  'set.llmGpu':     {en: 'Layers offloaded to the GPU (0 = CPU only, 999 = all of them)',
                     tr: 'GPU\'ya taşınacak katman (0 = sadece CPU, 999 = hepsi)'},
  'set.llmMaxTok':  {en: 'Maximum answer length (tokens)', tr: 'Azami yanıt uzunluğu (token)'},
  // Split around the two <code> spans in the markup: [1] .gguf [2] <dir> [3]
  'set.llmNote1':   {en: 'Drop a ', tr: 'Şu klasöre bir '},
  'set.llmNote2':   {en: ' into ', tr: ' koyun: '},
  'set.llmNote3':   {en: ' — "Scan" picks it up. A recording too long for the context window is summarized in chunks and merged.',
                     tr: ' — "Tara" ile listeye düşer. Kayıt bağlam penceresine sığmazsa parça parça özetlenip birleştirilir.'},
  'set.llmTimeout': {en: 'Summary timeout (s) — raise it for long or CPU-bound models',
                     tr: 'Özet zaman aşımı (sn) — uzun/CPU\'ya taşan modeller için artırın'},
  'set.vram':       {en: 'Sequence VRAM: unload the LLM during transcription and the STT model during summarizing (for 8 GB cards)',
                     tr: 'VRAM\'i sıraya koy: STT sırasında LLM\'i, özet sırasında STT modelini boşalt (8GB için)'},

  // -- llm model list / download note ---------------------------------------
  'llm.manualPath': {en: '(manually typed path)', tr: '(elle yazılan yol)'},
  'llm.noModel':    {en: '(no model found)', tr: '(model bulunamadı)'},
  'llm.autoFirst':  {en: '(automatic: the first model)', tr: '(otomatik: ilk model)'},
  'llm.downloaded': {en: ' · on disk', tr: ' · inik'},
  'llm.already':    {en: ' — already downloaded.', tr: ' — zaten indirilmiş.'},
  'llm.willDl':     {en: ' — to download: ', tr: ' — indirilecek: '},
  'llm.confirmDl':  {en: 'Download {label} ({size})?',
                     tr: '{label} indirilecek ({size}). Devam edilsin mi?'},
  'llm.downloading':{en: 'Downloading…', tr: 'İndiriliyor…'},
  'llm.ready':      {en: 'Model ready.', tr: 'Model hazır.'},
  'llm.starting':   {en: 'Starting the download…', tr: 'İndirme başlatılıyor…'},
};

let LANG = 'en';

// Looks up `key`, substituting {name} placeholders from `vars`.
function t(key, vars) {
  const e = STR[key];
  let s = e ? (e[LANG] || e.en) : key;
  if (vars) {
    Object.keys(vars).forEach(k => { s = s.split('{' + k + '}').join(vars[k]); });
  }
  return s;
}

function applyLang(lang) {
  LANG = (lang === 'tr') ? 'tr' : 'en';
  document.documentElement.lang = LANG;
  const set = (attr, fn) => {
    document.querySelectorAll('[' + attr + ']').forEach(el => {
      fn(el, t(el.getAttribute(attr)));
    });
  };
  set('data-i18n',       (el, s) => { el.textContent = s; });
  set('data-i18n-html',  (el, s) => { el.innerHTML = s; });
  set('data-i18n-title', (el, s) => { el.title = s; });
  set('data-i18n-ph',    (el, s) => { el.placeholder = s; });
  // Anything rendered by JS rather than sitting in the markup.
  if (window.afterLangChange) window.afterLangChange();
}

// Applied before app.js runs, so the first paint is already in the saved
// language; the server's stored value is reconciled on the first settings fetch.
(function () {
  let saved = 'en';
  try { saved = localStorage.getItem('transcriptor-lang') || 'en'; } catch (e) {}
  applyLang(saved);
})();
