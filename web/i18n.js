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

  // -- update banner --------------------------------------------------------
  'upd.k':          {en: 'Update', tr: 'Güncelleme'},
  'upd.new':        {en: 'A newer version is available:',
                     tr: 'Daha yeni bir sürüm var:'},
  'upd.get':        {en: 'Release notes', tr: 'Sürüm notları'},
  'upd.hide':       {en: 'Dismiss', tr: 'Kapat'},
  'upd.hideTitle':  {en: 'Hide this until the next release',
                     tr: 'Sonraki sürüme kadar gizle'},

  // -- console --------------------------------------------------------------
  'rec.start':      {en: 'Start recording', tr: 'Kaydı başlat'},
  'rec.stop':       {en: 'Stop recording', tr: 'Kaydı durdur'},
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
  // The same row when something in the folder could not be written: it only
  // says where the folder is, and "Not saved" above it says what is missing.
  'saved.folder':   {en: 'Folder →', tr: 'Klasör →'},
  // Shown when saving was asked for and did not happen: the take is in memory
  // only, and closing the window ends it.
  'saved.failed':   {en: 'Not saved →', tr: 'Kaydedilemedi →'},

  // -- status / notices -----------------------------------------------------
  'st.browserPaused': {en: 'Browser recording paused…',
                       tr: 'Tarayıcı kaydı duraklatıldı…'},
  'st.browserRec':    {en: 'Recording in the browser…',
                       tr: 'Tarayıcıda kayıt sürüyor…'},
  'st.pausedSuffix':  {en: ' · paused', tr: ' · duraklatıldı'},
  'note.sttMissing':  {en: '<b>No speech model is ready.</b> Nothing is downloaded on your behalf — click here to pick one in Settings and fetch it.',
                       tr: '<b>Hazır bir konuşma modeli yok.</b> Sizin adınıza hiçbir şey indirilmez — Ayarlar\'dan birini seçip indirmek için buraya tıklayın.'},
  'note.sttBtn':      {en: 'Pick and download a speech model in Settings first.',
                       tr: 'Önce Ayarlar\'dan bir konuşma modeli seçip indirin.'},
  'note.diarMissing': {en: '<b>The speaker-separation models are not downloaded yet.</b> They arrive on their own with the first recording (≈ 75 MB) — click here to fetch them now.',
                       tr: '<b>Konuşmacı ayrımı modelleri henüz inmedi.</b> İlk kayıtta kendiliğinden inecek (≈ 75 MB) — şimdi indirmek için buraya tıklayın.'},
  'note.diarRetry':   {en: ' — click to try again.', tr: ' — yeniden denemek için tıklayın.'},
  'note.diarUnbuilt': {en: '<b>This build was compiled without speaker separation.</b> The transcript comes out unlabelled.',
                       tr: '<b>Bu sürüm konuşmacı ayrımı olmadan derlendi.</b> Metin etiketsiz üretilir.'},

  // -- transcript / summary -------------------------------------------------
  'tx.label':       {en: 'Transcript', tr: 'Metin'},
  'tx.empty':       {en: 'Start and stop a recording — the transcript appears here.',
                     tr: 'Kaydı başlatıp durdurun — deşifre burada belirir.'},
  'tx.none':        {en: 'No transcript found.', tr: 'Metin bulunamadı.'},
  'tx.go':          {en: '✦ Transcribe', tr: '✦ Metne dönüştür'},
  'tx.goTitle':     {en: 'Run Whisper over the recording',
                     tr: 'Kaydı Whisper ile metne dönüştür'},
  'tx.waiting':     {en: 'Recorded — press Transcribe.',
                     tr: 'Kayıt alındı — Metne dönüştür\'e basın.'},
  'sum.label':      {en: 'Summary', tr: 'Özet'},
  'sum.tplTitle':   {en: 'Note template', tr: 'Not şablonu'},
  'sum.ctx':        {en: '＋ Context', tr: '＋ Bağlam'},
  'sum.ctxTitle':   {en: 'Add meeting context', tr: 'Toplantı bağlamı ekle'},
  'sum.go':         {en: '✦ Summarize', tr: '✦ Özetle'},
  // why the button is dim
  'sum.noTx':       {en: 'Nothing to summarize yet — transcribe a recording first.',
                     tr: 'Henüz özetlenecek bir şey yok — önce bir kaydı metne dönüştürün.'},
  'sum.emptyTx':    {en: 'The transcript has no text in it — there is nothing to summarize.',
                     tr: 'Metinde hiç söz yok — özetlenecek bir şey yok.'},
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
  // playback transport
  'pl.toggle':      {en: 'Play / pause', tr: 'Oynat / duraklat'},
  'pl.seek':        {en: 'Drag to seek — ← → jump 5s, with Shift 30s',
                     tr: 'Sürükleyerek ilerleyin — ← → 5 sn, Shift ile 30 sn'},
  'pl.mute':        {en: 'Mute / unmute', tr: 'Sesi kapat / aç'},
  'pl.rate':        {en: 'Playback speed', tr: 'Oynatma hızı'},
  'pl.follow':      {en: 'Follow the transcript — light the line being spoken and keep it on screen',
                     tr: 'Metni takip et — konuşulan satırı vurgula ve ekranda tut'},
  // running the models again over a saved recording
  'lib.retx':       {en: '↻ Transcribe', tr: '↻ Metne dönüştür'},
  'lib.retxTitle':  {en: 'Transcribe this recording again with the current model and settings',
                     tr: 'Bu kaydı geçerli model ve ayarlarla yeniden metne dönüştür'},
  'lib.resum':      {en: '↻ Summarize', tr: '↻ Özetle'},
  'lib.resumTitle': {en: 'Summarize the transcript shown here again, with the template chosen in the Studio',
                     tr: 'Burada görünen metni, Stüdyo\'da seçili şablonla yeniden özetle'},
  'lib.original':   {en: 'original', tr: 'özgün'},
  'lib.pickTx':     {en: 'Which saved transcript to show', tr: 'Hangi kayıtlı metin gösterilsin'},
  'lib.pickSum':    {en: 'Which saved summary to show', tr: 'Hangi kayıtlı özet gösterilsin'},
  'lib.runAskTx':   {en: 'This recording already has a transcript.',
                     tr: 'Bu kaydın zaten bir metni var.'},
  'lib.runAskSum':  {en: 'This recording already has a summary.',
                     tr: 'Bu kaydın zaten bir özeti var.'},
  'lib.runOverTx':  {en: 'Replace the transcript shown ({name})',
                     tr: 'Görünen metnin ({name}) üzerine yaz'},
  'lib.runOverSum': {en: 'Replace the summary shown ({name})',
                     tr: 'Görünen özetin ({name}) üzerine yaz'},
  'lib.runOverwrite': {en: 'Replace what is there', tr: 'Var olanın üzerine yaz'},
  'lib.runNew':     {en: 'Keep both, name the new one',
                     tr: 'İkisini de tut, yenisine ad ver'},
  'lib.runNamePh':  {en: 'second pass', tr: 'ikinci geçiş'},
  'lib.runStart':   {en: 'Run', tr: 'Çalıştır'},
  'lib.runNameNeeded': {en: 'Give the new version a name.',
                        tr: 'Yeni sürüme bir ad verin.'},
  'lib.runNameBad': {en: 'A name cannot contain . / \\ : < > " | ? or *',
                     tr: 'Ad şunları içeremez: . / \\ : < > " | ? *'},
  'lib.runningTx':  {en: 'Transcribing…', tr: 'Metne dönüştürülüyor…'},
  'lib.runningSum': {en: 'Summarizing…', tr: 'Özetleniyor…'},
  'sum.cutShort':   {en: 'Cut off at the maximum answer length — raise it in Settings → Advanced and summarize again.',
                     tr: 'Azami yanıt uzunluğunda kesildi — Ayarlar → Gelişmiş\'ten artırıp yeniden özetleyin.'},
  'lib.runStopped': {en: 'Stopped — nothing was written, and nothing was lost.',
                     tr: 'Durduruldu — hiçbir şey yazılmadı, hiçbir şey kaybolmadı.'},
  'lib.stop':       {en: '■ Stop', tr: '■ Durdur'},
  'lib.stopTitle':  {en: 'Stop the models. Nothing already saved is touched.',
                     tr: 'Modelleri durdur. Kaydedilmiş hiçbir şeye dokunulmaz.'},
  'lib.runDone':    {en: 'Done — the new version is selected.',
                     tr: 'Bitti — yeni sürüm seçildi.'},
  'lib.runFailed':  {en: 'That run did not finish.', tr: 'Bu çalıştırma tamamlanamadı.'},
  'lib.seekTo':     {en: 'Play the recording from here',
                     tr: 'Kaydı buradan oynat'},
  'lib.badges.tx':  {en: 'text', tr: 'metin'},
  'lib.badges.sum': {en: 'summary', tr: 'özet'},
  'lib.badges.aud': {en: 'audio', tr: 'ses'},
  'lib.loadErr':    {en: 'That recording could not be opened', tr: 'Kayıt açılamadı'},
  'lib.delete':     {en: '🗑 Delete', tr: '🗑 Sil'},
  'lib.deleteTitle':{en: 'Delete this recording folder and everything in it',
                     tr: 'Bu kaydın klasörünü içindekilerle birlikte sil'},
  // Bodies for the confirm dialog: the question itself is the dialog's title.
  'lib.deleteAsk':  {en: '{name}\n{path}\n\nThe folder and everything in it goes — audio, transcripts and summaries. This cannot be undone.',
                     tr: '{name}\n{path}\n\nKlasör ve içindeki her şey gider — ses, metinler ve özetler. Bu işlem geri alınamaz.'},
  'lib.deleted':    {en: 'Recording deleted', tr: 'Kayıt silindi'},

  // -- toasts ---------------------------------------------------------------
  'toast.cancelled':    {en: 'Cancelled', tr: 'İptal edildi'},
  'toast.jobStopped':   {en: 'Stopping — nothing was discarded',
                         tr: 'Durduruluyor — hiçbir şey silinmedi'},
  'toast.jobAlreadyDone': {en: 'The run finished while you were deciding — nothing was discarded',
                           tr: 'Siz karar verirken işlem bitti — hiçbir şey silinmedi'},
  'btn.cancelJob':      {en: 'Stop the run. The audio and transcript are kept.',
                         tr: 'Çalışmayı durdur. Ses ve metin korunur.'},
  'toast.uploading':    {en: 'Uploading: ', tr: 'Yükleniyor: '},
  'toast.uploadErr':    {en: 'Upload failed', tr: 'Yükleme hatası'},
  'toast.folderErr':    {en: 'Could not open the folder: ', tr: 'Klasör açılamadı: '},
  'toast.dlFailed':     {en: 'Download failed', tr: 'İndirme başarısız'},
  'toast.dlDone':       {en: 'Model downloaded', tr: 'Model indirildi'},
  'toast.dlCancelled':  {en: 'Download cancelled', tr: 'İndirme iptal edildi'},
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
  'toast.captureFailed':{en: 'Recording could not be started', tr: 'Kayıt başlatılamadı'},
  'toast.recError':     {en: 'The recording stopped on an error — what was captured was kept',
                         tr: 'Kayıt hatayla durdu — o ana kadarki kısım saklandı'},
  'toast.sourceEnded':  {en: 'The shared audio source ended — finishing the recording',
                         tr: 'Paylaşılan ses kaynağı sona erdi — kayıt tamamlanıyor'},
  'toast.pendingDropped': {en: 'Recording discarded', tr: 'Kayıt silindi'},
  'toast.resolvePending': {en: 'A recording is still waiting to be uploaded — retry, save or discard it first',
                           tr: 'Yüklenmeyi bekleyen bir kayıt var — önce yeniden deneyin, kaydedin ya da silin'},

  // -- a recording the server has not accepted yet --------------------------
  'pending.k':        {en: 'Not uploaded →', tr: 'Yüklenemedi →'},
  'pending.retry':    {en: 'Retry', tr: 'Yeniden dene'},
  'pending.save':     {en: 'Save a copy', tr: 'Kopyasını kaydet'},
  'pending.drop':     {en: 'Discard', tr: 'Sil'},
  'pending.dropAsk':  {en: 'It has not been saved anywhere else, so this is the only copy.',
                       tr: 'Başka hiçbir yere kaydedilmedi; elinizdeki tek kopya bu.'},

  // -- settings: shared -----------------------------------------------------
  'set.title':      {en: 'Settings', tr: 'Ayarlar'},
  // the page's own confirm dialog
  'ask.yes':          {en: 'OK', tr: 'Tamam'},
  'ask.downloadTitle':{en: 'Download this model?', tr: 'Bu model indirilsin mi?'},
  'ask.discardTitle': {en: 'Discard the recording?', tr: 'Kayıt silinsin mi?'},
  'ask.discard':      {en: 'Discard', tr: 'Sil'},
  'ask.deleteTitle':  {en: 'Delete this recording?', tr: 'Bu kayıt silinsin mi?'},

  // Cancel asks before it acts, and what it is about to give up depends on
  // what is running. The decline button says what carries on, never "Cancel" —
  // on a question about cancelling, that word answers nothing.
  'ask.cancelJobTitle':     {en: 'Stop the run?', tr: 'İşlem durdurulsun mu?'},
  'ask.cancelJob':          {en: 'The run stops where it is. The recording is kept, and so is any transcript it has already finished.',
                             tr: 'İşlem olduğu yerde durur. Kayıt ve o ana kadar tamamlanmış metin korunur.'},
  'ask.cancelJobYes':       {en: 'Stop the run', tr: 'İşlemi durdur'},
  'ask.cancelJobNo':        {en: 'Let it run', tr: 'Devam etsin'},

  'ask.cancelRec':          {en: 'Recording stops and the take is thrown away. It has not been saved anywhere yet.',
                             tr: 'Kayıt durur ve alınan ses atılır. Henüz hiçbir yere kaydedilmedi.'},
  'ask.cancelRecNo':        {en: 'Keep recording', tr: 'Kayda devam et'},

  'ask.cancelDiscardTitle': {en: 'Discard what is on screen?', tr: 'Ekrandakiler silinsin mi?'},
  'ask.cancelDiscard':      {en: 'The take, the transcript and the summary on screen are cleared. Files already written to the output folder stay where they are.',
                             tr: 'Ekrandaki kayıt, metin ve özet temizlenir. Çıktı klasörüne yazılmış dosyalar yerinde kalır.'},
  'ask.cancelDiscardNo':    {en: 'Keep it', tr: 'Kalsın'},

  'set.cancel':     {en: 'Cancel', tr: 'İptal'},
  'set.save':       {en: 'Save', tr: 'Kaydet'},

  // Tab labels: short on purpose — five of them share the modal width. The
  // longer "…Grp" strings below still title the group inside each tab.
  'set.tabGeneral': {en: 'General', tr: 'Genel'},
  'set.tabOutput':  {en: 'Output', tr: 'Çıktı'},
  'set.tabLlm':     {en: 'Summarizer', tr: 'Özetleyici'},
  'set.tabTpl':     {en: 'Templates', tr: 'Şablonlar'},
  'set.tabAdv':     {en: 'Advanced', tr: 'Gelişmiş'},

  'set.generalGrp': {en: 'General', tr: 'Genel'},
  'set.uiLang':     {en: 'Interface language', tr: 'Arayüz dili'},
  'set.theme':      {en: 'Appearance', tr: 'Görünüm'},
  'set.themeSystem':{en: 'Follow the system', tr: 'Sistemi izle'},
  'set.themeLight': {en: 'Light', tr: 'Aydınlık'},
  'set.themeDark':  {en: 'Dark', tr: 'Karanlık'},
  'set.lang':       {en: 'Spoken language', tr: 'Konuşma dili'},
  'set.autoDetect': {en: 'Auto-detect', tr: 'Otomatik algıla'},
  'set.sumLang':    {en: 'Summary language', tr: 'Özet dili'},
  'set.whisper':    {en: 'Speech model — pick one and download it',
                     tr: 'Konuşma modeli — birini seçip indirin'},
  'set.diarOn':     {en: 'Separate speakers', tr: 'Konuşmacıları ayır'},
  'set.updates':    {en: 'Check GitHub for new releases (once a day; nothing else is sent)',
                     tr: 'Yeni sürümler için GitHub\'a bak (günde bir; başka hiçbir şey gönderilmez)'},
  'set.version':    {en: 'This build:', tr: 'Bu sürüm:'},

  'set.outGrp':     {en: 'Output & Automation', tr: 'Çıktı & Otomasyon'},
  'set.outDir':     {en: 'Output folder', tr: 'Kayıt klasörü'},
  'set.saveAudio':  {en: 'Save the audio (audio.wav)', tr: 'Sesi kaydet (audio.wav)'},
  'set.saveTx':     {en: 'Save the transcript (transcript.txt/.json)',
                     tr: 'Metni kaydet (transcript.txt/.json)'},
  'set.saveSum':    {en: 'Save the summary (summary.txt)', tr: 'Özeti kaydet (summary.txt)'},
  'set.autoTx':     {en: 'Transcribe automatically when the recording stops',
                     tr: 'Kayıt bitince metne otomatik dönüştür'},
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
  'set.llmDlCancel':{en: '✕ Cancel', tr: '✕ İptal'},
  'set.llmDlCancelTitle': {en: 'Stop the download and delete the partial file',
                           tr: 'İndirmeyi durdur ve yarım dosyayı sil'},
  'llm.cancelling': {en: 'Cancelling…', tr: 'İptal ediliyor…'},
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
  'set.deviceNote': {en: 'Used for transcription and for the built-in summarizer. ' +
                         'Automatic prefers a discrete card over an integrated one.',
                     tr: 'Metne dönüştürme ve yerleşik özetleyici için kullanılır. ' +
                         'Otomatik, tümleşik yerine ayrık ekran kartını seçer.'},
  'set.integrated': {en: 'integrated', tr: 'tümleşik'},
  'set.deviceGone': {en: 'not found now', tr: 'şu an yok'},
  'set.modelFile':  {en: 'Model file (empty = the model chosen under General)',
                     tr: 'Model dosyası (boş = Genel\'de seçilen model)'},

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
  'set.clthr':      {en: 'Clustering threshold: {v}, from the transcription language. No longer a setting — the best value differs by language, and one number served neither.',
                     tr: 'Ayrım eşiği: {v}, konuşma dilinden geliyor. Artık bir ayar değil — en iyi değer dile göre değişiyor, tek bir sayı ikisine de uymuyordu.'},
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
  // speech model picker
  'stt.none':       {en: '— no model selected —', tr: '— model seçilmedi —'},
  'stt.notListed':  {en: ' · not in the list', tr: ' · listede yok'},
  'stt.pick':       {en: 'Pick a model to see what it is and what it costs.',
                     tr: 'Ne olduğunu ve boyutunu görmek için bir model seçin.'},
  'set.whisperDlBtn': {en: 'Download', tr: 'İndir'},
  'set.dlCancel':   {en: '✕ Cancel', tr: '✕ İptal'},
  'set.dlCancelTitle': {en: 'Stop the download and delete the partial file',
                        tr: 'İndirmeyi durdur ve yarım dosyayı sil'},
  'set.llmCtx':     {en: 'Context size (tokens) — 0 = automatic, from the model and the free memory',
                     tr: 'Bağlam boyutu (token) — 0 = otomatik, modele ve boştaki belleğe göre'},
  'set.llmGpu':     {en: 'Layers offloaded to the GPU (0 = CPU only, 999 = all of them)',
                     tr: 'GPU\'ya taşınacak katman (0 = sadece CPU, 999 = hepsi)'},
  'set.llmMaxTok':  {en: 'Maximum answer length (tokens)', tr: 'Azami yanıt uzunluğu (token)'},
  // Split around the two <code> spans in the markup: [1] .gguf [2] <dir> [3]
  'set.llmNote1':   {en: 'Drop a ', tr: 'Şu klasöre bir '},
  'set.llmNote2':   {en: ' into ', tr: ' koyun: '},
  'set.llmNote3':   {en: ' — "Scan" picks it up. A recording too long for the context window is summarized in chunks and merged.',
                     tr: ' — "Tara" ile listeye düşer. Kayıt bağlam penceresine sığmazsa parça parça özetlenip birleştirilir.'},
  'set.llmThink':   {en: 'Let the model think first: reasoning models may work through the final summary before writing it, on a budget of their own. Section notes are never written this way.',
                     tr: 'Önce düşünmesine izin ver: akıl yürüten modeller son özeti yazmadan önce, kendilerine ayrılan bütçeyle üzerinde düşünebilir. Bölüm notları hiçbir zaman böyle yazılmaz.'},
  'set.llmTimeout': {en: 'Summary timeout (s) — raise it for long or CPU-bound models',
                     tr: 'Özet zaman aşımı (sn) — uzun/CPU\'ya taşan modeller için artırın'},
  'set.vram':       {en: 'Sequence VRAM: unload the LLM during transcription and the STT model during summarizing',
                     tr: 'VRAM\'i sıraya koy: STT sırasında LLM\'i, özet sırasında STT modelini boşalt'},

  // -- llm model list / download note ---------------------------------------
  'llm.manualPath': {en: '(manually typed path)', tr: '(elle yazılan yol)'},
  'llm.noModel':    {en: '(no model found)', tr: '(model bulunamadı)'},
  'llm.autoFirst':  {en: '(automatic: the first model)', tr: '(otomatik: ilk model)'},
  'llm.downloaded': {en: ' · on disk', tr: ' · inik'},
  'llm.already':    {en: ' — already downloaded.', tr: ' — zaten indirilmiş.'},
  'llm.willDl':     {en: ' — to download: ', tr: ' — indirilecek: '},
  'llm.confirmDl':  {en: '{label}\n{size} will be fetched into the models folder. You can carry on using the app while it downloads.',
                     tr: '{label}\n{size} boyutunda, model klasörüne indirilecek. İnerken uygulamayı kullanmaya devam edebilirsiniz.'},
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
