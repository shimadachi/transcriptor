#include "app/shell.h"

#include <cstdio>

#ifdef TRANSCRIPTOR_HAVE_WEBVIEW
#  include "webview/webview.h"
#endif

#ifdef _WIN32
#  include <windows.h>
#  include <shellapi.h>
#else
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

bool run_window(const std::string& url, const std::string& title) {
#ifdef TRANSCRIPTOR_HAVE_WEBVIEW
    try {
        webview::webview window(/*debug=*/false, /*window=*/nullptr);
        window.set_title(title);
        window.set_size(1280, 860, WEBVIEW_HINT_NONE);
        window.set_size(900, 600, WEBVIEW_HINT_MIN);
        window.navigate(url);
        window.run();
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
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setsid();
        execlp(opener, opener, url.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, WNOHANG);
    return true;
#endif
}

}  // namespace transcriptor::app
