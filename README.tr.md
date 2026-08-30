# Transcriptor

[English](README.md) · **Türkçe**

[![License: MIT](https://img.shields.io/badge/License-MIT-e4491f.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)

Toplantı, ders ve görüşme kayıtlarını metne döken ve özetleyen **tek bir yerel
binary**. Kurulum, sanal ortam ya da harici bir servis gerekmez; Windows ve
macOS için doğrudan çalıştırılabilir dosya üretir.

**Ses hiçbir sunucuya gönderilmez; tüm işleme yereldir.**

## Neler yapar

- **Sistem sesini ya da mikrofonu kaydeder** — ikisini aynı anda karıştırabilir
  (gerçek zamanlı kazanç + tepe sınırlayıcı). Elinizdeki bir ses/video dosyasını
  da işleyebilir.
- **Metne döker** — whisper.cpp, kelime bazlı zaman damgalarıyla.
- **Konuşmacıları ayırır** — sherpa-onnx; "Konuşmacı 1/2/3" olarak etiketler.
- **Özetler** — gömülü llama.cpp ile, seçtiğiniz not şablonuna göre. Uzun
  kayıtlar parça parça özetlenip birleştirilir.
- **Not şablonları** — beş hazır şablon (toplantı, standup, ders, görüşme,
  genel) ve **kendi şablonlarınız**: ad + sistem promptu + kalıcı bağlam.
- **Arşiv** — çıktı klasöründeki tüm eski oturumları listeleyen ikinci sekme;
  metni, özeti ve saklanmış sesi oynatıcısıyla birlikte gösterir.
- **İki dilli arayüz** — English / Türkçe, seçim `config.json`'da saklanır.
  Hem dil hem aydınlık/karanlık tema Ayarlar → Genel altındadır.

## Bileşenler

| Katman | Kullanılan |
|---|---|
| Arayüz | HTML/CSS/JS, **native pencere** (WebView2 / WKWebView / WebKitGTK) |
| STT | **whisper.cpp** (ggml) |
| Konuşmacı ayrımı | **sherpa-onnx** + pyannote segmentation-3.0'ın ONNX hâli |
| Özetleyici | **Gömülü llama.cpp** (opsiyonel olarak LM Studio/Ollama da) |
| Ses yakalama | **miniaudio** (WASAPI / CoreAudio / PulseAudio) |
| HTTP | **cpp-httplib** |
| Dağıtım | **tek binary** (web arayüzü içine gömülü) |

## Gereksinimler

- CMake ≥ 3.21, Ninja (ya da Visual Studio 2022), C++17 derleyici
- Git (bağımlılıklar `FetchContent` ile kaynaktan derlenir)
- İlk derlemede ~2 GB indirme + 10–25 dk derleme süresi (llama.cpp + whisper.cpp
  + sherpa-onnx). Sonraki derlemeler saniyeler sürer.

Platforma özel:

- **Windows:** Visual Studio 2022 Build Tools. Pencere için **WebView2 Runtime**
  (Windows 11'de kurulu gelir; Windows 10'da Microsoft'un dağıttığı Evergreen
  runtime gerekir).
- **macOS:** Xcode Command Line Tools. Ek çalışma zamanı gerekmez.
- **Linux:** `libgtk-3-dev` + `libwebkit2gtk-4.1-dev` (native pencere için),
  PipeWire ya da PulseAudio.

## Derleme

```bash
# Linux
cmake --preset linux        && cmake --build --preset linux
cmake --preset linux-cuda   && cmake --build --preset linux-cuda     # NVIDIA
cmake --preset linux-vulkan && cmake --build --preset linux-vulkan   # AMD/Intel

# macOS (Apple Silicon — Metal açık)
cmake --preset mac-arm64    && cmake --build --preset mac-arm64
# macOS (Intel)
cmake --preset mac-x64      && cmake --build --preset mac-x64

# Windows
cmake --preset win-msvc     && cmake --build --preset win-msvc
cmake --preset win-cuda     && cmake --build --preset win-cuda       # NVIDIA
cmake --preset win-vulkan   && cmake --build --preset win-vulkan     # AMD/Intel
```

Her satırdaki iki komut da gerekli: ilki (`cmake --preset …`) yalnızca
yapılandırır, derlemeyi ikincisi (`cmake --build --preset …`) yapar. Her preset
kendi `build/<preset>/` klasörüne yazar; başka bir klasördeki eski binary
güncellenmez.

Web arayüzü (`web/`) binary'nin içine gömülür — `index.html`/`app.js`/`style.css`
değişince yeniden derlemeden arayüzde görünmez.

Çıktı:

- Linux: `build/<preset>/transcriptor`
- macOS: `build/<preset>/transcriptor.app`
- Windows: `build/<preset>/Release/transcriptor.exe`

Dağıtılabilir paket için: `cd build/<preset> && cpack`
(Windows → `.zip`, macOS → `.dmg`, Linux → `.tar.gz`).

### Hazır yapılar (CI)

Her preset kendi iş akışında, kendi işletim sistemi üzerinde derlenir. Rozete
tıklayınca o hedefin çalışmalarına gidersiniz; paketler her çalışmanın
**Artifacts** bölümünden inebilir.

| Preset | Runner | Durum |
|---|---|---|
| `linux` | ubuntu-latest | [![linux](https://github.com/shimadachi/transcriptor/actions/workflows/linux.yml/badge.svg)](https://github.com/shimadachi/transcriptor/actions/workflows/linux.yml) |
| `linux-cuda` | ubuntu-latest | [![linux-cuda](https://github.com/shimadachi/transcriptor/actions/workflows/linux-cuda.yml/badge.svg)](https://github.com/shimadachi/transcriptor/actions/workflows/linux-cuda.yml) |
| `linux-vulkan` | ubuntu-latest | [![linux-vulkan](https://github.com/shimadachi/transcriptor/actions/workflows/linux-vulkan.yml/badge.svg)](https://github.com/shimadachi/transcriptor/actions/workflows/linux-vulkan.yml) |
| `mac-arm64` | macos-14 | [![mac-arm64](https://github.com/shimadachi/transcriptor/actions/workflows/mac-arm64.yml/badge.svg)](https://github.com/shimadachi/transcriptor/actions/workflows/mac-arm64.yml) |
| `mac-x64` | macos-15-intel | [![mac-x64](https://github.com/shimadachi/transcriptor/actions/workflows/mac-x64.yml/badge.svg)](https://github.com/shimadachi/transcriptor/actions/workflows/mac-x64.yml) |
| `win-msvc` | windows-2022 | [![win-msvc](https://github.com/shimadachi/transcriptor/actions/workflows/win-msvc.yml/badge.svg)](https://github.com/shimadachi/transcriptor/actions/workflows/win-msvc.yml) |
| `win-cuda` | windows-2022 | [![win-cuda](https://github.com/shimadachi/transcriptor/actions/workflows/win-cuda.yml/badge.svg)](https://github.com/shimadachi/transcriptor/actions/workflows/win-cuda.yml) |
| `win-vulkan` | windows-2022 | [![win-vulkan](https://github.com/shimadachi/transcriptor/actions/workflows/win-vulkan.yml/badge.svg)](https://github.com/shimadachi/transcriptor/actions/workflows/win-vulkan.yml) |

CI yalnızca derler, çalıştırmaz; GPU yapıları için ilgili CUDA ya da Vulkan
sürücüsüne sahip bir makine gerekir.

CUDA presetini **derlemek** için `nvcc` PATH'te olmalı (ya da
`-DCMAKE_CUDA_COMPILER=/nvcc/yolu`). **Çalıştırmak** için gerekmez.

CUDA paketleri kendi `cudart` / `cublas` / `cublasLt` kütüphanelerini içinde
taşır; bunlar sürücüyle değil CUDA **toolkit**'iyle gelir. Aksi hâlde yalnızca
sürücüsü olan ya da farklı ana sürüm toolkit'i olan bir makinede uygulama hiç
açılmaz (`libcudart.so.12: cannot open shared object file`,
`cudart64_12.dll was not found`). CUDA arşivlerinin ~450 MB, CPU arşivinin
~15 MB olmasının sebebi budur. Yerelde derlerseniz kendi toolkit'inize
bağlanır; hiçbir şey paketlenmez, binary küçük kalır.

### Derleme seçenekleri

| Seçenek | Varsayılan | Ne yapar |
|---|---|---|
| `TRANSCRIPTOR_DIARIZE` | ON | Konuşmacı ayrımı (sherpa-onnx + ONNX Runtime). OFF ederseniz derleme hızlanır, metin etiketsiz üretilir. |
| `TRANSCRIPTOR_WEBVIEW` | ON | Native pencere. OFF → tarayıcıda açılır. |
| `TRANSCRIPTOR_CUDA` / `TRANSCRIPTOR_VULKAN` / `TRANSCRIPTOR_METAL` | OFF / OFF / macOS'ta ON | GPU hızlandırma. Hem STT hem özetleyici aynı ayarı kullanır. |
| `TRANSCRIPTOR_NATIVE` | OFF | Yalnızca x86. ON, CPU backend'ini `-march=native` ile derler; daha hızlıdır ama *sadece* onu derleyen makineye benzer bir CPU'da çalışır. Derlemenin taşınabilir kalması için varsayılan kapalıdır; başkasına vereceğiniz bir binary için asla açmayın. |

## Çalıştırma

Binary'yi çalıştırın; pencere açılır. Konsoldan:

```bash
./transcriptor              # pencereyi aç
./transcriptor --check      # başsız teşhis (cihaz, ses kaynakları, modeller)
./transcriptor --no-window  # sadece sunucu; varsayılan tarayıcıda aç
./transcriptor --port 5005
```

## Modeller

Hiçbir model binary'ye gömülü değildir; ilk ihtiyaç duyulduğunda otomatik iner
(indirme için `curl` kullanılır — Windows 10 1803+, macOS ve Linux'ta hazır
gelir). İnenler:

| Model | Boyut | Ne zaman |
|---|---|---|
| `ggml-large-v3.bin` (whisper.cpp) | ~3.1 GB | İlk kayıt işlenirken |
| pyannote segmentation-3.0 (ONNX) | ~6 MB | İlk konuşmacı ayrımında |
| 3D-Speaker ERes2NetV2 ses izi | ~69 MB | İlk konuşmacı ayrımında |

Konum: `%APPDATA%\Transcriptor\models` (Windows),
`~/Library/Application Support/Transcriptor/models` (macOS),
`~/.config/Transcriptor/models` (Linux). `TRANSCRIPTOR_MODELS_DIR` ile değiştirilir.

Önceden indirmek için: `./scripts/fetch_models.sh` ya da
`.\scripts\fetch_models.ps1`.

**pyannote için HuggingFace token gerekmez** — kullanılan ONNX çıktısı açık
erişimlidir.

### Özetleyici modeli (GGUF)

Gömülü llama.cpp bir GGUF dosyası bekler. En kolayı Ayarlar → Özetleyici →
**Hazır model indir**: listeden bir model seçip **İndir** deyin, dosya modeller
klasörüne inip otomatik olarak seçilir. Hiçbiri binary'ye gömülü değildir,
indirme isteğe bağlıdır.

| Model | Boyut | Not |
| --- | --- | --- |
| Qwen3.5 4B Instruct `Q4_K_M` | ~2.6 GB | Önerilen — kalite/boyut dengesi |
| Qwen3.5 2B Instruct `Q4_K_M` | ~1.2 GB | 4 GB VRAM ya da salt CPU |
| Qwen3.5 0.8B Instruct `Q4_K_M` | ~0.5 GB | En küçük, en kaba özet |
| Gemma 4 E2B Instruct `Q4_K_M` | ~2.9 GB | Google Gemma 4, küçük sürüm |
| Gemma 4 E4B Instruct `Q4_K_M` | ~4.6 GB | 8 GB VRAM ister |
| Qwen2.5 7B Instruct `Q4_K_M` | ~4.4 GB | Eski varsayılan |

Elle indirmeyi tercih ederseniz modeller klasörüne bir `.gguf` koyup Ayarlar →
Özetleyici → **Tara** demeniz de yeterli.

LM Studio'yu tercih ederseniz Ayarlar → Özetleyici → **Uzak sunucu** seçip
`http://127.0.0.1:1234/v1` yazın.

## Ses kaydı

- **Windows:** WASAPI loopback — herhangi bir çıkış aygıtının sesi doğrudan
  kaydedilir. Kaynak listesinde tüm hoparlörler görünür.
- **Linux:** PipeWire/PulseAudio monitor kaynakları loopback olarak listelenir.
- **macOS:** İşletim sistemi sistem sesini doğrudan kaydettirmez. Sanal bir
  çıkış aygıtı gerekir:
  ```bash
  brew install blackhole-2ch
  ```
  Ardından Ses Ayarları'ndan çoklu çıkış aygıtı kurup kaynak olarak BlackHole'ü
  seçin. Uygulama bunu algılar ve loopback olarak işaretler; kurulu değilse
  arayüzde açıklayıcı bir uyarı çıkar. (Mikrofon kaydı her platformda çalışır.)

Kaynak satırından ikinci bir **mikrofon** seçerseniz sistem sesine gerçek
zamanlı karıştırılır; kazançlar ve tepe sınırlayıcı ayarlardan yönetilir.

## VRAM yönetimi

8 GB kart için modeller sırayla yüklenir: modelleri uygulama tuttuğu için STT
başlamadan önce LLM ağırlıkları, özet başlamadan önce whisper ağırlıkları
doğrudan bırakılır.
Ayarlar → **VRAM'i sıraya koy** ile kapatılabilir.

Uzun kayıtlar bağlam penceresine sığmazsa özetleyici otomatik olarak parça
parça not çıkarıp sonra bunları birleştirir.

## Yapılandırma

`config.json`, modellerle aynı klasörün üstünde tutulur. Ortam değişkenleri:
`TRANSCRIPTOR_HOST`, `TRANSCRIPTOR_PORT`, `TRANSCRIPTOR_OUTPUT_DIR`, `TRANSCRIPTOR_MODELS_DIR`, `TRANSCRIPTOR_LLM_BASE_URL`,
`TRANSCRIPTOR_LLM_MODEL`, `TRANSCRIPTOR_LLM_MODEL_PATH`, `TRANSCRIPTOR_WHISPER_MODEL`, `TRANSCRIPTOR_DEVICE`,
`TRANSCRIPTOR_NO_DIARIZE`, `TRANSCRIPTOR_LLAMA_VERBOSE`.

## Bağımlılık sürümlerini yükseltmek

Tüm sürümler `cmake/dependencies.cmake` başındaki `TRANSCRIPTOR_*_TAG` değişkenlerinde.
Dikkat edilmesi gereken tek nokta: **llama.cpp ve whisper.cpp aynı ggml'i
paylaşır.** llama.cpp önce çekilir ve `ggml` hedeflerini tanımlar; whisper.cpp
mevcut `ggml` hedefini yeniden kullanır. İkisini birbirinden çok uzak tarihlere
sabitlerseniz ggml API'si uyuşmaz. İkisini birlikte, yakın tarihli sürümlere
yükseltin.

llama.cpp'nin C API'si de sık değişir; `src/llm/llama_backend.cpp` mevcut
`llama_model_load_from_file` / `llama_init_from_model` / `llama_sampler_chain`
API'sini kullanır. Çok eski bir tag'e dönerseniz burası uyarlanmalıdır.

**sherpa-onnx** gömülmek üzere tasarlanmamış: CMakeLists'i `CMAKE_SOURCE_DIR`'i
kendi kökü sanıyor. `cmake/dependencies.cmake` bunu üç yamayla çözer —
(1) `SOURCE_SUBDIR` hilesiyle önce indirip sonra elle `add_subdirectory`,
(2) kendi `cmake/` klasörünü `CMAKE_MODULE_PATH`'e ekleme,
(3) kaynak kökünü `include_directories`'e ekleme. Ayrıca alt bağımlılıkları
CMake 4'ün reddettiği eski `cmake_minimum_required` sürümlerini bildirdiği için
o alt ağaca `CMAKE_POLICY_VERSION_MINIMUM=3.5` verilir. Sürüm yükseltirken
bunların hâlâ gerekli (ya da yeterli) olup olmadığını kontrol edin.

Konuşmacı ayrımı sherpa-onnx'in **C** API'siyle yazıldı (`c-api.h`); C++
sarmalayıcısı (`cxx-api.h`) diarization'ı kapsamıyor.

## Proje yapısı

```
src/
  main.cpp              Giriş noktası, --check, pencere/tarayıcı seçimi
  config.*              JSON ayarlar (Settings)
  device.*              GPU tespiti (ggml backend kayıt defteri üzerinden)
  audio/                miniaudio: kaynak listesi, yakalama, kayıt/mixleme, dosya çözme
  stt/whisper_stt.*     whisper.cpp sarmalayıcı, kelime zaman damgalı
  diarize/diarizer.*    sherpa-onnx konuşmacı ayrımı
  llm/                  şablonlar + gömülü llama.cpp + OpenAI uyumlu istemci
  pipeline/processor.*  transcribe → diarize → konuşmacı eşleme
  app/                  AppState, /api/* sunucusu, native pencere, gömülü varlıklar
  util/                 yollar, dışa aktarma, arşiv, dil, indirme, model kayıt defteri
web/                    Arayüz (derleme sırasında binary'ye gömülür)
  index.html            İşaretleme; çevrilecek metinler data-i18n ile etiketli
  app.js                Tüm arayüz mantığı
  i18n.js               en/tr sözlüğü ve dil uygulayıcı
  style.css             Tema (aydınlık/karanlık), düzen
```

## Not şablonları

Ayarlar → **Not şablonları** altından hazır şablonların sistem promptunu
düzenleyebilir, her birine kalıcı bir bağlam ekleyebilir (örn. "Şirketimiz
Acme; kararlara odaklan") ya da **+ Yeni şablon** ile kendi şablonunuzu
yazabilirsiniz. Kendi şablonlarınız hazır olanların yanında menüde görünür ve
`config.json` içinde `custom_templates` altında saklanır.

## Arşiv

**Arşiv** sekmesi, çıktı klasöründeki tüm oturum klasörlerini en yeniden eskiye
listeler; seçtiğinizi gösterir: metin (konuşmacı ve zaman damgalarıyla, doğrudan
`transcript.json`'dan), özet ve saklanmış sesin oynatıcısı. Hiçbir şey
indekslenmez — veritabanı diskteki klasörün kendisidir; dışarıdan kopyaladığınız
bir oturum listede belirir, sildiğiniz kaybolur.

## Ayarlar

Ayarlar genelden özele sıralanır. **Genel** (arayüz dili, görünüm, konuşma ve
özet dili, Whisper modeli, konuşmacı ayrımı), **Çıktı & Otomasyon**,
**Özetleyici** ve **Not şablonları** üstte durur; yalnızca ince ayar
gerektiğinde önemli olan her şey — cihaz seçimi, model dosya yolları,
kazançlar, ayrım eşiği, bağlam boyutu, GPU katmanları — en alttaki
**Gelişmiş** başlığının altındadır.

## Arayüz dili

Ayarlar → **Genel** → **Arayüz dili**. Seçim `config.json`'daki `ui_language`
alanına yazılır, ilk boyamada titremesin diye `localStorage`'da da tutulur;
aydınlık/karanlık tema (`ui_theme`, `system` değerini de alır) aynı şekilde
çalışır. Varsayılan İngilizce'dir. Özet dili (`summary_language`) ayrıdır:
arayüz İngilizce, özetler Türkçe olabilir.

## Lisans

[MIT](LICENSE) — © 2026 shimadachi.

Derlenen binary'nin içindeki üçüncü taraf bileşenler kendi lisanslarıyla
gelir: llama.cpp ve whisper.cpp (MIT), sherpa-onnx (Apache-2.0), ONNX Runtime
(MIT), miniaudio (MIT/Unlicense), cpp-httplib (MIT), nlohmann/json (MIT),
webview (MIT), Eigen (MPL-2.0), OpenFST (Apache-2.0).
