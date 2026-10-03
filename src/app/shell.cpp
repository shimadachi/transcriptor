#include "app/shell.h"

#include <cstdio>
#include <memory>

#ifdef TRANSCRIPTOR_HAVE_WEBVIEW
#  include <nlohmann/json.hpp>
#  include "webview/webview.h"
#endif

#ifdef _WIN32
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <errno.h>
#  include <unistd.h>
#  include <sys/wait.h>
#endif

namespace transcriptor::app {

bool has_native_window() {
#ifdef TRANSCRIPTOR_HAVE_WEBVIEW
    return true;
#else
    return false;
#endif
}

#ifdef TRANSCRIPTOR_HAVE_WEBVIEW
namespace {

// What the page's bindings share with the tray. Held by shared_ptr because
// webview drains its queue while it is being destroyed, and a binding call
// still in there would otherwise land on locals that are already gone.
struct WindowState {
    // A take recorded in the page itself (browser capture), which the app's
    // recorder never sees. web/app.js reports it through trayReport().
    bool page_rec    = false;
    bool page_paused = false;
    bool page_busy   = false;   // asking for permission, or uploading the take

    std::unique_ptr<Tray> tray;
};

}  // namespace
#endif

bool run_window(const std::string& url, const std::string& title,
                std::function<TrayStatus()> recorder) {
#ifdef TRANSCRIPTOR_HAVE_WEBVIEW
    try {
        webview::webview window(/*debug=*/false, /*window=*/nullptr);
        window.set_title(title);
        window.set_size(1280, 860, WEBVIEW_HINT_NONE);
        window.set_size(900, 600, WEBVIEW_HINT_MIN);

        auto shared = std::make_shared<WindowState>();
        // Raw, not shared: the tray is owned by `shared`, and a copy of the
        // pointer inside it would keep the pair alive for ever.
        WindowState* ws = shared.get();

        auto status = [ws, &recorder] {
            TrayStatus s = recorder ? recorder() : TrayStatus{};
            if (ws->page_rec) {
                s.recording = true;
                s.paused = ws->page_paused;
            }
            if (ws->page_busy) s.can_record = false;
            return s;
        };
        // Start/Stop and Pause press the page's own buttons (trayAction in
        // web/app.js) rather than calling the API from here: the source to
        // record from is chosen in the page, and so is a capture the page
        // records itself.
        auto command = [&window](TrayCommand c) {
            switch (c) {
            case TrayCommand::kRecord:
                window.eval("window.trayAction && window.trayAction('record')");
                break;
            case TrayCommand::kPause:
                window.eval("window.trayAction && window.trayAction('pause')");
                break;
            case TrayCommand::kQuit:
                window.terminate();   // the same way out as closing the window
                break;
            case TrayCommand::kShowHide:
                break;                // the tray deals with the window itself
            }
        };
        const auto native = window.window();
        if (Tray::supported() && native.ok()) {
            shared->tray = std::make_unique<Tray>(native.value(), status, command);
        }

        // Bound only alongside a tray: the page shows the tray setting when it
        // finds these, so a build or a platform without one does not offer it.
        if (shared->tray) {
            window.bind("trayReport", [shared](const std::string& args) -> std::string {
                const auto parsed =
                    nlohmann::json::parse(args, nullptr, /*allow_exceptions=*/false);
                if (parsed.is_array() && !parsed.empty() && parsed[0].is_object()) {
                    const nlohmann::json& take = parsed[0];
                    const auto flag = [&take](const char* key) {
                        const auto it = take.find(key);
                        return it != take.end() && it->is_boolean() && it->get<bool>();
                    };
                    shared->page_rec = flag("rec");
                    shared->page_paused = flag("paused");
                    shared->page_busy = flag("busy");
                }
                if (shared->tray) shared->tray->refresh();
                return "";
            });
            window.bind("trayShow", [shared](const std::string&) -> std::string {
                if (shared->tray) shared->tray->show_window();
                return "";
            });
        }

        window.navigate(url);
        window.run();
        // Before the window goes, not with it: the tray hooks that window, and
        // its menu's commands reach into it.
        shared->tray.reset();
        return true;
    } catch (const std::exception& e) {
        // Missing WebView2 runtime on Windows, or no WebKitGTK on Linux.
        std::fprintf(stderr, "Native window unavailable (%s); using the browser.\n",
                     e.what());
        return false;
    }
#else
    (void)url;
    (void)title;
    (void)recorder;
    return false;
#endif
}

bool open_in_browser(const std::string& url) {
#ifdef _WIN32
    const std::wstring wide(url.begin(), url.end());   // URLs are ASCII
    HINSTANCE rc = ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr,
                                 SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(rc) > 32;
#else
#  ifdef __APPLE__
    const char* opener = "open";
#  else
    const char* opener = "xdg-open";
#  endif
    // Double-fork. The middle process exits the moment it has spawned the
    // opener, so the blocking wait below returns at once and reaps it, while
    // the opener itself is orphaned to init and outlives the app -- which is
    // the point of the setsid(). A single fork reaped with WNOHANG reaped
    // nothing at all: that soon after forking the child is always still alive,
    // so every click left a <defunct> entry behind for the rest of the session.
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setsid();
        if (fork() == 0) {
            execlp(opener, opener, url.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        _exit(0);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    return true;
#endif
}

}  // namespace transcriptor::app
