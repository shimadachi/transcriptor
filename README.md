# Transcriptor

**English** · [Türkçe](README.tr.md)

[![License: MIT](https://img.shields.io/badge/License-MIT-e4491f.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)

Transcribes and summarizes meetings, lectures, and interviews as **a single
local binary**. No install, no virtualenv, no external service; it produces a
directly runnable executable for Windows and macOS.

**Audio is never sent to any server; all processing is local.**

## What it does

- **Records system audio or a microphone** — or both at once, mixed live with
  gain control and a peak limiter. It can also process an audio/video file you
  already have.
- **Transcribes** — whisper.cpp, with word-level timestamps.
- **Separates speakers** — sherpa-onnx, labelling them "Speaker 1/2/3".
- **Summarizes** — embedded llama.cpp, following the note template you pick.
  Recordings too long for the context window are summarized in chunks and merged.
- **Note templates** — five built in (meeting, standup, lecture, interview,
  general) plus **your own**: name, system prompt, and persistent context.
- **Library** — a second tab listing every past session in the output folder,
  with its transcript, its summary, and a player for the saved audio.
- **Bilingual interface** — English / Turkish, remembered in `config.json`.
  Both the language and the light / dark theme live in Settings → General.

## Components

| Layer | Used |
|---|---|
| Interface | HTML/CSS/JS in a **native window** (WebView2 / WKWebView / WebKitGTK) |
| STT | **whisper.cpp** (ggml) |
| Speaker separation | **sherpa-onnx** + the ONNX build of pyannote segmentation-3.0 |
| Summarizer | **Embedded llama.cpp** (optionally LM Studio / Ollama) |
| Audio capture | **miniaudio** (WASAPI / CoreAudio / PulseAudio) |
| HTTP | **cpp-httplib** |
| Distribution | **one binary**, web UI compiled in |

## Requirements

- CMake ≥ 3.21, Ninja (or Visual Studio 2022), a C++17 compiler
- Git (dependencies are built from source via `FetchContent`)
- First build pulls ~2 GB and takes 10–25 min (llama.cpp + whisper.cpp +
  sherpa-onnx). Later builds take seconds.

Platform specific:

- **Windows:** Visual Studio 2022 Build Tools. The window needs the **WebView2
  Runtime** (preinstalled on Windows 11; on Windows 10 install Microsoft's
  Evergreen runtime).
- **macOS:** Xcode Command Line Tools. No extra runtime.
- **Linux:** `libgtk-3-dev` + `libwebkit2gtk-4.1-dev` for the native window,
  and PipeWire or PulseAudio.

## Building

```bash
# Linux
cmake --preset linux        && cmake --build --preset linux
cmake --preset linux-cuda   && cmake --build --preset linux-cuda     # NVIDIA
cmake --preset linux-vulkan && cmake --build --preset linux-vulkan   # AMD/Intel

# macOS (Apple Silicon — Metal on)
cmake --preset mac-arm64    && cmake --build --preset mac-arm64
# macOS (Intel)
cmake --preset mac-x64      && cmake --build --preset mac-x64

# Windows
cmake --preset win-msvc     && cmake --build --preset win-msvc
cmake --preset win-cuda     && cmake --build --preset win-cuda       # NVIDIA
cmake --preset win-vulkan   && cmake --build --preset win-vulkan     # AMD/Intel
```

Both commands on each line are needed: the first (`cmake --preset …`) only
configures, the second (`cmake --build --preset …`) compiles. Every preset
writes to its own `build/<preset>/`, so a binary in another folder is not
updated.

The web UI (`web/`) is compiled into the binary — changes to
`index.html`/`app.js`/`style.css` do not show up without a rebuild.

Output:

- Linux: `build/<preset>/transcriptor`
- macOS: `build/<preset>/transcriptor.app`
- Windows: `build/<preset>/Release/transcriptor.exe`

For a distributable package: `cd build/<preset> && cpack`
(Windows → `.zip`, macOS → `.dmg`, Linux → `.tar.gz`).

### Prebuilt (CI)

Every preset builds in its own workflow on its own OS. Click a badge to reach
that target's runs; packages download from each run's **Artifacts** section.

| Preset | Runner | Status |
|---|---|---|
| `linux` | ubuntu-latest | [![linux](https://github.com/shimadachi/transcriptor/actions/workflows/linux.yml/badge.svg)](https://github.com/shimadachi/transcriptor/actions/workflows/linux.yml) |
| `linux-cuda` | ubuntu-latest | [![linux-cuda](https://github.com/shimadachi/transcriptor/actions/workflows/linux-cuda.yml/badge.svg)](https://github.com/shimadachi/transcriptor/actions/workflows/linux-cuda.yml) |
| `linux-vulkan` | ubuntu-latest | [![linux-vulkan](https://github.com/shimadachi/transcriptor/actions/workflows/linux-vulkan.yml/badge.svg)](https://github.com/shimadachi/transcriptor/actions/workflows/linux-vulkan.yml) |
| `mac-arm64` | macos-14 | [![mac-arm64](https://github.com/shimadachi/transcriptor/actions/workflows/mac-arm64.yml/badge.svg)](https://github.com/shimadachi/transcriptor/actions/workflows/mac-arm64.yml) |
| `mac-x64` | macos-15-intel | [![mac-x64](https://github.com/shimadachi/transcriptor/actions/workflows/mac-x64.yml/badge.svg)](https://github.com/shimadachi/transcriptor/actions/workflows/mac-x64.yml) |
| `win-msvc` | windows-2022 | [![win-msvc](https://github.com/shimadachi/transcriptor/actions/workflows/win-msvc.yml/badge.svg)](https://github.com/shimadachi/transcriptor/actions/workflows/win-msvc.yml) |
| `win-cuda` | windows-2022 | [![win-cuda](https://github.com/shimadachi/transcriptor/actions/workflows/win-cuda.yml/badge.svg)](https://github.com/shimadachi/transcriptor/actions/workflows/win-cuda.yml) |
| `win-vulkan` | windows-2022 | [![win-vulkan](https://github.com/shimadachi/transcriptor/actions/workflows/win-vulkan.yml/badge.svg)](https://github.com/shimadachi/transcriptor/actions/workflows/win-vulkan.yml) |

CI only compiles; it never runs the GPU builds. Those need a machine with the
matching CUDA or Vulkan driver.

**Building** a CUDA preset needs `nvcc` on `PATH` (or
`-DCMAKE_CUDA_COMPILER=/path/to/nvcc`). **Running** the result does not.

The CUDA packages carry their own `cudart` / `cublas` / `cublasLt`, because
those come with the CUDA *toolkit* and not with the driver — without them the
binary will not start on a machine that has only a driver, or a toolkit of a
different major version (`libcudart.so.12: cannot open shared object file`,
`cudart64_12.dll was not found`). That is why the CUDA archives are ~450 MB
against ~15 MB for the CPU one. Building locally links against your own
toolkit, so nothing is bundled and the binary stays small.

### Build options

| Option | Default | Effect |
|---|---|---|
| `TRANSCRIPTOR_DIARIZE` | ON | Speaker separation (sherpa-onnx + ONNX Runtime). OFF builds faster and produces unlabelled transcripts. |
| `TRANSCRIPTOR_WEBVIEW` | ON | Native window. OFF → opens in the browser. |
| `TRANSCRIPTOR_CUDA` / `TRANSCRIPTOR_VULKAN` / `TRANSCRIPTOR_METAL` | OFF / OFF / ON on macOS | GPU acceleration. STT and the summarizer share the setting. |

## Running

Run the binary and a window opens. From a console:

```bash
./transcriptor              # open the window
./transcriptor --check      # headless diagnostics (device, audio sources, models)
./transcriptor --no-window  # server only; opens your default browser
./transcriptor --port 5005
```

## Models

No model is bundled; each downloads the first time it is needed (via `curl`,
which ships with Windows 10 1803+, macOS, and Linux):

| Model | Size | When |
|---|---|---|
| `ggml-large-v3.bin` (whisper.cpp) | ~3.1 GB | First recording processed |
| pyannote segmentation-3.0 (ONNX) | ~6 MB | First speaker separation |
| 3D-Speaker ERes2NetV2 embedding | ~69 MB | First speaker separation |

Location: `%APPDATA%\Transcriptor\models` (Windows),
`~/Library/Application Support/Transcriptor/models` (macOS),
`~/.config/Transcriptor/models` (Linux). Override with
`TRANSCRIPTOR_MODELS_DIR`.

To fetch them ahead of time: `./scripts/fetch_models.sh` or
`.\scripts\fetch_models.ps1`.

**No HuggingFace token is needed for pyannote** — the ONNX export used here is
openly accessible.

### Summarizer model (GGUF)

The embedded llama.cpp expects a GGUF file. The easiest route is Settings →
Summarizer → **Download a ready-made model**: pick one and click **Download**;
it lands in the models folder and is selected automatically. Nothing is
bundled, and downloading is optional.

| Model | Size | Note |
| --- | --- | --- |
| Qwen3.5 4B Instruct `Q4_K_M` | ~2.6 GB | Recommended — quality/size balance |
| Qwen3.5 2B Instruct `Q4_K_M` | ~1.2 GB | 4 GB VRAM, or CPU only |
| Qwen3.5 0.8B Instruct `Q4_K_M` | ~0.5 GB | Smallest, roughest summaries |
| Gemma 4 E2B Instruct `Q4_K_M` | ~2.9 GB | Google Gemma 4, small variant |
| Gemma 4 E4B Instruct `Q4_K_M` | ~4.6 GB | Wants 8 GB VRAM |
| Qwen2.5 7B Instruct `Q4_K_M` | ~4.4 GB | Previous default |

If you prefer to download by hand, drop a `.gguf` into the models folder and
hit Settings → Summarizer → **Scan**.

To use LM Studio instead, pick Settings → Summarizer → **Remote server** and
enter `http://127.0.0.1:1234/v1`.

## Audio capture

- **Windows:** WASAPI loopback — any output device is recorded directly. Every
  speaker shows up in the source list.
- **Linux:** PipeWire/PulseAudio monitor sources are listed as loopbacks.
- **macOS:** the OS will not let you record system audio directly. A virtual
  output device is required:
  ```bash
  brew install blackhole-2ch
  ```
  Then create a multi-output device in Audio Settings and pick BlackHole as the
  source. The app detects it and marks it as a loopback; if it is missing, the
  UI explains why. (Microphone capture works on every platform.)

Selecting a second **microphone** in the source row mixes it into the system
audio in real time; gains and the peak limiter come from Settings.

## VRAM management

For 8 GB cards the models load in sequence: because the app holds the weights
itself, the LLM is released before transcription starts and whisper is released
before summarizing does. Turn it off with Settings → **Sequence VRAM**.

When a recording does not fit the context window, the summarizer takes notes
chunk by chunk and merges them.

## Configuration

`config.json` sits one level above the models folder. Environment variables:
`TRANSCRIPTOR_HOST`, `TRANSCRIPTOR_PORT`, `TRANSCRIPTOR_OUTPUT_DIR`,
`TRANSCRIPTOR_MODELS_DIR`, `TRANSCRIPTOR_LLM_BASE_URL`,
`TRANSCRIPTOR_LLM_MODEL`, `TRANSCRIPTOR_LLM_MODEL_PATH`,
`TRANSCRIPTOR_WHISPER_MODEL`, `TRANSCRIPTOR_DEVICE`,
`TRANSCRIPTOR_NO_DIARIZE`, `TRANSCRIPTOR_LLAMA_VERBOSE`.

## Note templates

Settings → **Note templates** lets you edit a built-in template's system
prompt, attach persistent context to it (e.g. "Our company is Acme; focus on
decisions"), or write your own with **+ New template**. Your templates appear
in the menu next to the built-ins and are stored under `custom_templates` in
`config.json`.

## Library

The **Library** tab lists every session folder under the output directory,
newest first, and shows the one you pick: the transcript (with speakers and
timestamps, straight from `transcript.json`), the summary, and a player for
whatever audio was saved. Nothing is indexed — the folder on disk is the whole
database, so a session you copy in appears and one you delete disappears.

## Settings

Settings runs general to specific. **General** (interface language, appearance,
spoken and summary language, Whisper model, speaker separation), **Output &
Automation**, **Summarizer** and **Note templates** sit at the top; everything
that only matters when something needs tuning — device selection, model file
paths, gains, clustering thresholds, context size, GPU layers — is folded into
**Advanced** at the bottom.

## Interface language

Settings → **General** → **Interface language**. The choice is written to
`ui_language` in `config.json`, and mirrored in `localStorage` so the first
paint does not flash; the light/dark theme (`ui_theme`, which also accepts
`system`) works the same way. English is the default. Summary language
(`summary_language`) is separate: an English interface can produce Turkish
summaries.

## Upgrading dependency versions

All versions live in the `TRANSCRIPTOR_*_TAG` variables at the top of
`cmake/dependencies.cmake`. The one thing to watch: **llama.cpp and whisper.cpp
share the same ggml.** llama.cpp is fetched first and defines the `ggml`
targets; whisper.cpp reuses the existing one. Pin them to dates far apart and
the ggml API will not match. Upgrade them together, to nearby versions.

llama.cpp's C API also changes often; `src/llm/llama_backend.cpp` uses the
current `llama_model_load_from_file` / `llama_init_from_model` /
`llama_sampler_chain` API. Going back to a much older tag means adapting it.

**sherpa-onnx** was not designed to be embedded: its CMakeLists assumes
`CMAKE_SOURCE_DIR` is its own root. `cmake/dependencies.cmake` works around
this three ways — (1) the `SOURCE_SUBDIR` trick to download first and
`add_subdirectory` manually, (2) adding its own `cmake/` folder to
`CMAKE_MODULE_PATH`, (3) adding its source root to `include_directories`. Its
sub-dependencies also declare `cmake_minimum_required` versions CMake 4
rejects, so that subtree gets `CMAKE_POLICY_VERSION_MINIMUM=3.5`. Check whether
these are still necessary (or sufficient) when upgrading.

Speaker separation is written against sherpa-onnx's **C** API (`c-api.h`); the
C++ wrapper (`cxx-api.h`) does not cover diarization.

## Project layout

```
src/
  main.cpp              Entry point, --check, window/browser choice
  config.*              JSON settings (Settings)
  device.*              GPU detection (via the ggml backend registry)
  audio/                miniaudio: source list, capture, recording/mixing, decoding
  stt/whisper_stt.*     whisper.cpp wrapper, word-level timestamps
  diarize/diarizer.*    sherpa-onnx speaker separation
  llm/                  templates + embedded llama.cpp + OpenAI-compatible client
  pipeline/processor.*  transcribe → diarize → speaker attribution
  app/                  AppState, the /api/* server, native window, embedded assets
  util/                 paths, export, library, language, downloads, model registry
web/                    Interface (compiled into the binary at build time)
  index.html            Markup; translatable strings tagged with data-i18n
  app.js                All interface logic
  i18n.js               en/tr dictionary and the language applier
  style.css             Theme (light/dark), layout
```

## License

[MIT](LICENSE) — © 2026 shimadachi.

Third-party components inside the compiled binary keep their own licenses:
llama.cpp and whisper.cpp (MIT), sherpa-onnx (Apache-2.0), ONNX Runtime (MIT),
miniaudio (MIT/Unlicense), cpp-httplib (MIT), nlohmann/json (MIT), webview
(MIT), Eigen (MPL-2.0), OpenFST (Apache-2.0).
