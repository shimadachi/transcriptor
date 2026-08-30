// Entry point: start the local server, then show the window.
//
// Everything runs on this machine. The server binds to loopback and exists
// only so the native window has something to load — no audio, transcript or
// summary ever leaves the process except to the files you choose to save.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "app/server.h"
#include "app/shell.h"
#include "app/state.h"
#include "audio/sources.h"
#include "config.h"
#include "device.h"
#include "diarize/diarizer.h"
#include "llm/llama_backend.h"
#include "util/lang.h"
#include "util/models.h"
#include "util/net.h"

#ifdef _WIN32
#  include <windows.h>
#  include <shellapi.h>   // CommandLineToArgvW, used by wWinMain below
#endif

namespace {

using namespace transcriptor;

struct Options {
    bool check      = false;   // headless diagnostics, then exit
    bool no_window  = false;   // serve only; open the default browser
    bool show_help  = false;
    bool show_version = false;
    int  port       = -1;      // -1 = use the configured port
};

Options parse_args(const std::vector<std::string>& args) {
    Options opts;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--check")            opts.check = true;
        else if (a == "--no-window" || a == "--browser") opts.no_window = true;
        else if (a == "--help" || a == "-h") opts.show_help = true;
        else if (a == "--version")     opts.show_version = true;
        else if (a == "--port" && i + 1 < args.size()) {
            opts.port = std::atoi(args[++i].c_str());
        }
    }
    return opts;
}

// English only: --help runs before config.json is read, so there is no
// interface language to follow yet.
void print_help() {
    std::printf(
        "transcriptor " TRANSCRIPTOR_VERSION "\n"
        "  Records system audio, transcribes it offline, separates the speakers\n"
        "  and summarizes with a local LLM. No data ever leaves the machine.\n\n"
        "Usage: transcriptor [options]\n\n"
        "  --check        Headless diagnostics (device, audio sources, models)\n"
        "  --no-window    Do not open a window; use the browser\n"
        "  --port <n>     Local server port\n"
        "  --version      Print the version\n"
        "  --help         This help\n");
}

// `--check`: a quick, dependency-free health report.
int run_check(const Settings& settings) {
    std::printf("transcriptor " TRANSCRIPTOR_VERSION "\n\n");

    const DeviceInfo device = resolve_device(settings.device, settings.compute_type);
    std::printf(L("Device       : %s\n", "Cihaz        : %s\n"),
                device.badge().c_str());
    std::printf(L("  backend    : %s (GPUs: %d)\n",
                  "  backend    : %s (GPU sayısı: %d)\n"),
                device.backend.c_str(), device.gpu_count);
    if (device.vram_total > 0) {
        std::printf("  VRAM       : %s\n",
                    models::human_size(device.vram_total).c_str());
    }

    std::printf(L("\nConfig       : %s\n", "\nYapılandırma : %s\n"),
                paths::to_utf8(Settings::config_path()).c_str());
    std::printf(L("Models       : %s\n", "Modeller     : %s\n"),
                paths::to_utf8(paths::models_dir()).c_str());
    std::printf(L("Recordings   : %s\n", "Kayıtlar     : %s\n"),
                settings.output_dir.c_str());

    std::printf(L("\nAudio sources:\n", "\nSes kaynakları:\n"));
    try {
        const auto sources = audio::list_sources();
        if (sources.empty()) std::printf(L("  (none)\n", "  (yok)\n"));
        for (const auto& s : sources) {
            std::printf("  %s %s\n",
                        s.is_loopback ? L("[system]", "[sistem]")
                                      : L("[microphone]", "[mikrofon]"),
                        s.name.c_str());
        }
    } catch (const std::exception& e) {
        std::printf(L("  ERROR: %s\n", "  HATA: %s\n"), e.what());
    }
    const std::string hint = audio::macos_loopback_hint();
    if (!hint.empty()) std::printf("  ! %s\n", hint.c_str());

    const char* ready    = L("ready", "hazır");
    const char* will_get  = L("will be downloaded", "indirilecek");

    std::printf(L("\nModels:\n", "\nModeller:\n"));
    std::printf("  Whisper (%s): %s\n", settings.whisper_model.c_str(),
                models::whisper_ready(settings) ? ready : will_get);
    if (diarize::Diarizer::supported()) {
        std::printf(L("  Speaker separation : %s\n", "  Konuşmacı ayrımı : %s\n"),
                    models::diarization_ready(settings) ? ready : will_get);
    } else {
        std::printf(L("  Speaker separation : not compiled in\n",
                      "  Konuşmacı ayrımı : derlenmemiş\n"));
    }
    std::printf(L("  Download tool      : %s\n", "  İndirme aracı    : %s\n"),
                net::can_download()
                    ? L("curl found", "curl var")
                    : L("MISSING (place the models by hand)",
                        "YOK (modelleri elle koyun)"));

    std::printf(L("\nSummarizer (%s):\n", "\nÖzetleyici (%s):\n"),
                settings.llm_backend.c_str());
    if (settings.llm_backend == "embedded") {
        const auto ggufs = llm::discover_gguf_models();
        if (ggufs.empty()) {
            std::printf(L("  No model — drop a .gguf into the models folder.\n",
                          "  Model yok — models klasörüne bir .gguf koyun.\n"));
        }
        for (const std::string& g : ggufs) std::printf("  %s\n", g.c_str());
    } else {
        std::printf(L("  Server: %s\n", "  Sunucu: %s\n"),
                    settings.llm_base_url.c_str());
    }

    app::AppState probe(settings);
    try {
        for (const std::string& m : probe.list_llm_models("")) {
            std::printf("  -> %s\n", m.c_str());
        }
    } catch (const std::exception& e) {
        std::printf(L("  ERROR: %s\n", "  HATA: %s\n"), e.what());
    }
    return 0;
}

int run(const std::vector<std::string>& args) {
    const Options opts = parse_args(args);

    if (opts.show_help)    { print_help(); return 0; }
    if (opts.show_version) { std::printf("%s\n", TRANSCRIPTOR_VERSION); return 0; }

    Settings settings = Settings::load();
    if (opts.port > 0) settings.port = opts.port;

    if (opts.check) return run_check(settings);

    app::AppState state(settings);
    app::Server server(&state, settings.host, settings.port);

    if (!server.start()) {
        std::fprintf(stderr,
                     L("The local server could not be started (%s:%d).\n",
                       "Yerel sunucu başlatılamadı (%s:%d).\n"),
                     settings.host.c_str(), settings.port);
        return 1;
    }

    const std::string url = server.base_url();
    std::printf("transcriptor: %s\n", url.c_str());

    bool windowed = false;
    if (!opts.no_window) {
        windowed = app::run_window(
            url, L("Transcriptor · audio · text · summary",
                   "Transcriptor · ses · metin · özet"));
    }

    if (!windowed) {
        // No native window: hand off to the browser and keep serving until the
        // process is killed (Ctrl-C, or the launcher closing).
        app::open_in_browser(url);
        std::printf(L("No window available; opened in the browser. Ctrl-C to quit.\n",
                      "Pencere açılamadı; tarayıcıda açıldı. Kapatmak için Ctrl-C.\n"));
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    state.shutdown();
    server.stop();
    return 0;
}

}  // namespace

#if defined(_WIN32)

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv_w = CommandLineToArgvW(GetCommandLineW(), &argc);

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.push_back(paths::to_utf8(paths::fs::path(argv_w[i])));
    }
    LocalFree(argv_w);

    // The GUI subsystem has no console; attach the parent's for --check/--help
    // so running it from a terminal still prints.
    const bool wants_console = std::any_of(
        args.begin(), args.end(), [](const std::string& a) {
            return a == "--check" || a == "--help" || a == "-h" || a == "--version";
        });
    if (wants_console && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
    }
    return run(args);
}

#else

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    return run(args);
}

#endif
