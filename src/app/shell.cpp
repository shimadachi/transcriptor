#include "app/shell.h"

#ifdef _WIN32
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <errno.h>
#  include <unistd.h>
#  include <sys/wait.h>
#endif

namespace transcriptor::app {

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
