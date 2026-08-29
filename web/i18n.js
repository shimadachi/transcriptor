// ---- UI language (tr / en) ----
// Turkish stays the source language: it is what sits inline in index.html, so
// the page reads correctly before this file runs. Every visible string has an
// entry here; applyLang() rewrites the DOM from it.
//
// Markup hooks:  data-i18n="key"        -> textContent
//                data-i18n-html="key"   -> innerHTML (strings with tags)
//                data-i18n-title="key"  -> title attribute
//                data-i18n-ph="key"     -> placeholder attribute

const STR = {
  // -- header ---------------------------------------------------------------
  'hdr.theme':      {tr: 'Aydınlık / karanlık tema', en: 'Light / dark theme'},
  'hdr.lang':       {tr: 'Arayüz dili: Türkçe → English', en: 'Interface language: English → Türkçe'},
  'hdr.settings':   {tr: '⚙ Ayarlar', en: '⚙ Settings'},

  // -- console --------------------------------------------------------------
  'rec.start':      {tr: 'Kaydı başlat', en: 'Start recording'},
  'rec.recording':  {tr: 'Kayıtta', en: 'Recording'},
  'src.label':      {tr: 'Kaynak', en: 'Source'},
  'src.micMix':     {tr: 'Mikrofonu bu kaynağa karıştır', en: 'Mix a microphone into this source'},
  'src.refresh':    {tr: 'Yenile', en: 'Refresh'},
  'src.file':       {tr: '⇪ Dosya', en: '⇪ File'},
  'src.fileTitle':  {tr: 'Kayıtlı ses dosyası işle', en: 'Process a saved audio file'},
  'src.pause':      {tr: '⏸ Duraklat', en: '⏸ Pause'},
  'src.resume':     {tr: '▶ Sürdür', en: '▶ Resume'},
  'src.pauseTitle': {tr: 'Duraklat/Sürdür', en: 'Pause / resume'},
  'src.cancel':     {tr: '✕ İptal', en: '✕ Cancel'},
  'src.cancelTitle':{tr: 'Kaydı/çıktıyı iptal et ve sil', en: 'Cancel and delete the recording / output'},
  'src.stop':       {tr: '■ Durdur', en: '■ Stop'},
  'src.ready':      {tr: 'Hazır', en: 'Ready'},
  'src.browserBoth':  {tr: '🎙 Tarayıcı — Mikrofon + Sistem', en: '🎙 Browser — Microphone + System'},
  'src.browserSys':   {tr: '🖥 Tarayıcı — Sistem / Sekme sesi', en: '🖥 Browser — System / Tab audio'},
  'src.browserMic':   {tr: '🎤 Tarayıcı — Mikrofon', en: '🎤 Browser — Microphone'},
  'src.micNoneAvail': {tr: '+ Mik: yok', en: '+ Mic: none'},
  'src.micNone':      {tr: '+ Mik yok', en: '+ No mic'},
  'saved.k':        {tr: 'Kaydedildi →', en: 'Saved →'},
  'saved.open':     {tr: 'Klasörü Aç', en: 'Open Folder'},

  // -- status / notices -----------------------------------------------------
  'st.browserPaused': {tr: 'Tarayıcı kaydı duraklatıldı…', en: 'Browser recording paused…'},
  'st.browserRec':    {tr: 'Tarayıcıda kayıt sürüyor…', en: 'Recording in the browser…'},
  'st.pausedSuffix':  {tr: ' · duraklatıldı', en: ' · paused'},
  'note.sttMissing':  {tr: '<b>Metin modeli henüz inmedi.</b> İlk kayıtta otomatik indirilecek (large-v3 ≈ 3 GB).',
                       en: '<b>The transcription model is not downloaded yet.</b> It downloads automatically on the first recording (large-v3 ≈ 3 GB).'},
  'note.diarMissing': {tr: '<b>Konuşmacı ayrımı modelleri henüz inmedi.</b> İlk kayıtta otomatik indirilecek (≈ 75 MB).',
                       en: '<b>The speaker-separation models are not downloaded yet.</b> They download automatically on the first recording (≈ 75 MB).'},
  'note.diarUnbuilt': {tr: '<b>Bu sürüm konuşmacı ayrımı olmadan derlendi.</b> Metin etiketsiz üretilir.',
                       en: '<b>This build was compiled without speaker separation.</b> The transcript comes out unlabelled.'},

  // -- transcript / summary -------------------------------------------------
  'tx.label':       {tr: 'Metin', en: 'Transcript'},
  'tx.empty':       {tr: 'Kaydı başlatıp durdurun — deşifre burada belirir.',
                     en: 'Start and stop a recording — the transcript appears here.'},
  'tx.none':        {tr: 'Metin bulunamadı.', en: 'No transcript found.'},
  'sum.label':      {tr: 'Özet', en: 'Summary'},
  'sum.tplTitle':   {tr: 'Not şablonu', en: 'Note template'},
  'sum.ctx':        {tr: '＋ Bağlam', en: '＋ Context'},
  'sum.ctxTitle':   {tr: 'Toplantı bağlamı ekle', en: 'Add meeting context'},
  'sum.go':         {tr: '✦ Özetle', en: '✦ Summarize'},
  'sum.phTitle':    {tr: 'Başlık (ör. Q3 Bütçe Toplantısı)', en: 'Title (e.g. Q3 Budget Meeting)'},
  'sum.phPeople':   {tr: 'Katılımcılar (virgülle ayırın)', en: 'Participants (comma separated)'},
  'sum.phNotes':    {tr: 'Ek bağlam / gündem / özel talimat — özete dahil edilir',
                     en: 'Extra context / agenda / special instructions — included in the summary'},

  // -- toasts ---------------------------------------------------------------
  'toast.cancelled':    {tr: 'İptal edildi', en: 'Cancelled'},
  'toast.uploading':    {tr: 'Yükleniyor: ', en: 'Uploading: '},
  'toast.uploadErr':    {tr: 'Yükleme hatası', en: 'Upload failed'},
  'toast.folderErr':    {tr: 'Klasör açılamadı: ', en: 'Could not open the folder: '},
  'toast.dlFailed':     {tr: 'İndirme başarısız', en: 'Download failed'},
  'toast.dlDone':       {tr: 'Model indirildi', en: 'Model downloaded'},
  'toast.dlAlready':    {tr: 'Model zaten inik — seçildi', en: 'Model already downloaded — selected'},
  'toast.modelsFound':  {tr: ' model bulundu', en: ' model(s) found'},
  'toast.noModel':      {tr: 'Model bulunamadı', en: 'No model found'},
  'toast.saved':        {tr: 'Ayarlar kaydedildi', en: 'Settings saved'},
  'toast.savedDropped': {tr: 'Ayarlar kaydedildi · yönergesi boş {n} şablon atlandı',
                         en: 'Settings saved · skipped {n} template(s) with an empty prompt'},
  'toast.noBrowserRec': {tr: 'Tarayıcı kaydı desteklenmiyor (güvenli bağlam / localhost gerekir)',
                         en: 'Browser recording is not supported (needs a secure context / localhost)'},
  'toast.noPermission': {tr: 'İzin verilmedi veya desteklenmiyor', en: 'Permission denied, or not supported'},
  'toast.noAudio':      {tr: 'Ses alınamadı. Linux\'ta yalnızca bir SEKME sesi paylaşılabilir (pencere/ekran değil) — bir sekme seçip "sesi paylaş"ı işaretleyin, ya da kaynaktan sistem (loopback) seçin.',
                         en: 'No audio captured. On Linux only a TAB\'s audio can be shared (not a window/screen) — pick a tab and tick "share audio", or choose the system (loopback) source instead.'},
  'toast.recCancelled': {tr: 'Kayıt iptal edildi', en: 'Recording cancelled'},
  'toast.emptyRec':     {tr: 'Boş kayıt', en: 'Empty recording'},
  'toast.processing':   {tr: 'İşleniyor…', en: 'Processing…'},

  // -- settings: shared -----------------------------------------------------
  'set.title':      {tr: 'Ayarlar', en: 'Settings'},
  'set.cancel':     {tr: 'İptal', en: 'Cancel'},
  'set.save':       {tr: 'Kaydet', en: 'Save'},

  'set.sttGrp':     {tr: 'Metin · STT', en: 'Transcription · STT'},
  'set.device':     {tr: 'Cihaz', en: 'Device'},
  'set.auto':       {tr: 'Otomatik', en: 'Automatic'},
  'set.whisper':    {tr: 'Whisper modeli', en: 'Whisper model'},
  'set.lang':       {tr: 'Dil', en: 'Language'},
  'set.autoDetect': {tr: 'Otomatik algıla', en: 'Auto-detect'},
  'set.modelFile':  {tr: 'Model dosyası (boş = otomatik indir)', en: 'Model file (empty = download automatically)'},

  'set.mixGrp':     {tr: 'Ses karışımı · Mikrofon + Sistem', en: 'Audio mix · Microphone + System'},
  'set.sysGain':    {tr: 'Sistem sesi kazancı (0–4)', en: 'System audio gain (0–4)'},
  'set.micGain':    {tr: 'Mikrofon kazancı (0–4)', en: 'Microphone gain (0–4)'},
  'set.mixNote':    {tr: 'Kaynak satırında bir mikrofon seçince sistem sesine karıştırılır. Kırpılmayı önlemek için tepe sınırlayıcı otomatik devrededir.',
                     en: 'Picking a microphone in the source row mixes it into the system audio. A peak limiter is applied automatically to prevent clipping.'},

  'set.diarGrp':    {tr: 'Konuşmacı ayrımı · sherpa-onnx', en: 'Speaker separation · sherpa-onnx'},
  'set.diarOn':     {tr: 'Konuşmacıları ayır', en: 'Separate speakers'},
  'set.nspk':       {tr: 'Konuşmacı sayısı (0 = otomatik bul)', en: 'Number of speakers (0 = detect automatically)'},
  'set.clthr':      {tr: 'Ayrım eşiği (0.05–0.95) — düşük değer daha çok konuşmacı üretir',
                     en: 'Clustering threshold (0.05–0.95) — a lower value yields more speakers'},
  'set.segModel':   {tr: 'Bölütleme modeli (boş = otomatik indir)', en: 'Segmentation model (empty = download automatically)'},
  'set.embModel':   {tr: 'Ses izi modeli (boş = otomatik indir)', en: 'Speaker-embedding model (empty = download automatically)'},
  'set.diarNote1':  {tr: 'pyannote segmentation-3.0 modelinin ONNX hâli kullanılır — token gerekmez, çıkarım tamamen yerel. Modeller ilk kullanımda',
                     en: 'The ONNX build of pyannote segmentation-3.0 is used — no token needed, inference is fully local. The models download on first use into'},
  'set.diarNote2':  {tr: 'klasörüne iner.', en: '.'},

  'set.outGrp':     {tr: 'Çıktı & Otomasyon', en: 'Output & Automation'},
  'set.outDir':     {tr: 'Kayıt klasörü', en: 'Output folder'},
  'set.saveAudio':  {tr: 'Sesi kaydet (audio.wav)', en: 'Save the audio (audio.wav)'},
  'set.saveTx':     {tr: 'Metni kaydet (transcript.txt/.json)', en: 'Save the transcript (transcript.txt/.json)'},
  'set.saveSum':    {tr: 'Özeti kaydet (summary.txt)', en: 'Save the summary (summary.txt)'},
  'set.autoSum':    {tr: 'İşleme bitince özeti otomatik çıkar', en: 'Summarize automatically when processing finishes'},
  'set.vram':       {tr: 'VRAM\'i sıraya koy: STT sırasında LLM\'i, özet sırasında STT modelini boşalt (8GB için)',
                     en: 'Sequence VRAM: unload the LLM during transcription and the STT model during summarizing (for 8 GB cards)'},

  'set.llmGrp':     {tr: 'Özetleyici · llama.cpp', en: 'Summarizer · llama.cpp'},
  'set.llmMode':    {tr: 'Çalışma biçimi', en: 'Mode'},
  'set.llmEmbedded':{tr: 'Gömülü (llama.cpp) — kurulum gerekmez', en: 'Embedded (llama.cpp) — nothing to install'},
  'set.llmRemote':  {tr: 'Uzak sunucu (LM Studio / Ollama / llama-server)', en: 'Remote server (LM Studio / Ollama / llama-server)'},
  'set.llmDl':      {tr: 'Hazır model indir (isteğe bağlı, tek seferlik)', en: 'Download a ready-made model (optional, one time)'},
  'set.llmDlBtn':   {tr: 'İndir', en: 'Download'},
  'set.gguf':       {tr: 'GGUF modeli', en: 'GGUF model'},
  'set.scan':       {tr: 'Tara', en: 'Scan'},
  'set.llmPath':    {tr: 'Model dosya yolu (listede yoksa elle yazın)', en: 'Model file path (type it if it is not in the list)'},
  'set.llmCtx':     {tr: 'Bağlam boyutu (token)', en: 'Context size (tokens)'},
  'set.llmGpu':     {tr: 'GPU\'ya taşınacak katman (0 = sadece CPU, 999 = hepsi)',
                     en: 'Layers offloaded to the GPU (0 = CPU only, 999 = all of them)'},
  'set.llmMaxTok':  {tr: 'Azami yanıt uzunluğu (token)', en: 'Maximum answer length (tokens)'},
  // Split around the two <code> spans in the markup: [1] .gguf [2] <dir> [3]
  'set.llmNote1':   {tr: 'Şu klasöre bir ', en: 'Drop a '},
  'set.llmNote2':   {tr: ' koyun: ', en: ' into '},
  'set.llmNote3':   {tr: ' — "Tara" ile listeye düşer. Kayıt bağlam penceresine sığmazsa parça parça özetlenip birleştirilir.',
                     en: ' — "Scan" picks it up. A recording too long for the context window is summarized in chunks and merged.'},
  'set.llmUrl':     {tr: 'Sunucu URL', en: 'Server URL'},
  'set.model':      {tr: 'Model', en: 'Model'},
  'set.fetch':      {tr: 'Getir', en: 'Fetch'},
  'set.llmTimeout': {tr: 'Özet zaman aşımı (sn) — uzun/CPU\'ya taşan modeller için artırın',
                     en: 'Summary timeout (s) — raise it for long or CPU-bound models'},
  'set.sumLang':    {tr: 'Özet dili', en: 'Summary language'},
  'set.uiLang':     {tr: 'Arayüz dili', en: 'Interface language'},

  'set.tplGrp':     {tr: 'Not şablonları · düzenle', en: 'Note templates · edit'},
  'set.tplPick':    {tr: 'Düzenlenecek şablon', en: 'Template to edit'},
  'set.tplName':    {tr: 'Şablon adı', en: 'Template name'},
  'set.tplNamePh':  {tr: 'Örn. Müşteri Görüşmesi', en: 'e.g. Customer Call'},
  'set.tplPrompt':  {tr: 'Şablon yönergesi (özetleyiciye verilen sistem promptu)',
                     en: 'Template instruction (the system prompt given to the summarizer)'},
  'set.tplCtx':     {tr: 'Kalıcı ek bağlam (bu şablonun her özetine eklenir)',
                     en: 'Persistent extra context (added to every summary from this template)'},
  'set.tplCtxPh':   {tr: 'Örn. Şirketimiz Acme; kararlara ve aksiyonlara odaklan.',
                     en: 'e.g. Our company is Acme; focus on decisions and action items.'},
  'set.tplNew':     {tr: '+ Yeni şablon', en: '+ New template'},
  'set.tplReset':   {tr: 'Yönergeyi varsayılana sıfırla', en: 'Reset the instruction to default'},
  'set.tplDel':     {tr: 'Sil', en: 'Delete'},
  'set.tplUntitled':{tr: 'Şablon', en: 'Template'},
  'set.tplNewName': {tr: 'Yeni şablon', en: 'New template'},

  // -- llm model list / download note ---------------------------------------
  'llm.manualPath': {tr: '(elle yazılan yol)', en: '(manually typed path)'},
  'llm.noModel':    {tr: '(model bulunamadı)', en: '(no model found)'},
  'llm.already':    {tr: ' — zaten indirilmiş.', en: ' — already downloaded.'},
  'llm.downloading':{tr: 'İndiriliyor…', en: 'Downloading…'},
  'llm.ready':      {tr: 'Model hazır.', en: 'Model ready.'},
  'llm.starting':   {tr: 'İndirme başlatılıyor…', en: 'Starting the download…'},
};

let LANG = 'tr';

// Looks up `key`, substituting {name} placeholders from `vars`.
function t(key, vars) {
  const e = STR[key];
  let s = e ? (e[LANG] || e.tr) : key;
  if (vars) {
    Object.keys(vars).forEach(k => { s = s.split('{' + k + '}').join(vars[k]); });
  }
  return s;
}

function applyLang(lang) {
  LANG = (lang === 'en') ? 'en' : 'tr';
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
  let saved = 'tr';
  try { saved = localStorage.getItem('transcriptor-lang') || 'tr'; } catch (e) {}
  applyLang(saved);
})();
