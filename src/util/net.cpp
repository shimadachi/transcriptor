#include "util/net.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "util/lang.h"

#ifdef _WIN32
#  include <windows.h>
#else
#  include <errno.h>
#  include <poll.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  ifdef __linux__
#    include <sys/prctl.h>   // PR_SET_PDEATHSIG
#  endif
#endif

namespace transcriptor::net {

namespace {

constexpr std::size_t kMaxCapturedOutput = 8192;

// Kill a child the hard way. curl has no polite stop, and by the time anyone
// cancels, the useful state is the .part file the caller is about to delete.
void kill_child(std::intptr_t child) {
#ifdef _WIN32
    TerminateProcess(reinterpret_cast<HANDLE>(child), 1);
#else
    ::kill(static_cast<pid_t>(child), SIGKILL);
#endif
}

#ifdef _WIN32

// argv -> a single command line with the quoting rules CommandLineToArgvW uses.
std::wstring build_command_line(const std::vector<std::string>& argv) {
    std::wstring out;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) out += L' ';
        std::wstring arg = paths::from_utf8(argv[i]).wstring();
        bool needs_quotes = arg.empty() ||
                            arg.find_first_of(L" \t\"") != std::wstring::npos;
        if (!needs_quotes) { out += arg; continue; }
        out += L'"';
        for (std::size_t j = 0; j < arg.size(); ++j) {
            std::size_t slashes = 0;
            while (j < arg.size() && arg[j] == L'\\') { ++slashes; ++j; }
            if (j == arg.size()) {
                out.append(slashes * 2, L'\\');
                break;
            }
            if (arg[j] == L'"') {
                out.append(slashes * 2 + 1, L'\\');
            } else {
                out.append(slashes, L'\\');
            }
            out += arg[j];
        }
        out += L'"';
    }
    return out;
}

ProcResult run_win(const std::vector<std::string>& argv, double timeout_sec,
                   Canceller* cancel) {
    ProcResult res;
    // Already cancelled: never start the child at all.
    if (cancel && cancel->requested()) { res.cancelled = true; return res; }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return res;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = write_end;
    si.hStdError  = write_end;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring cmd = build_command_line(argv);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(write_end);
    if (!ok) {
        CloseHandle(read_end);
        return res;
    }
    res.launched = true;

    // Same window as the POSIX path: a cancel that landed between the check at
    // the top and CreateProcessW must still take this child down. Terminating
    // it also unblocks the ReadFile below, which is what makes cancel work here
    // even though that loop has no timeout of its own.
    if (cancel && !cancel->adopt(reinterpret_cast<std::intptr_t>(pi.hProcess))) {
        TerminateProcess(pi.hProcess, 1);
    }

    char chunk[1024];
    DWORD got = 0;
    while (ReadFile(read_end, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
        if (res.output.size() < kMaxCapturedOutput) res.output.append(chunk, got);
    }
    CloseHandle(read_end);

    DWORD wait_ms = timeout_sec > 0 ? static_cast<DWORD>(timeout_sec * 1000)
                                    : INFINITE;
    if (WaitForSingleObject(pi.hProcess, wait_ms) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        res.exit_code = -2;
    } else {
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        res.exit_code = static_cast<int>(code);
    }
    // Before the handle is closed, for the same reason the POSIX path releases
    // before reaping: a cancel must never reach a handle that is already gone.
    if (cancel) cancel->release();
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (cancel && cancel->requested()) res.cancelled = true;
    return res;
}

#else

ProcResult run_posix(const std::vector<std::string>& argv, double timeout_sec,
                     Canceller* cancel) {
    ProcResult res;
    if (argv.empty()) return res;
    // Already cancelled: never start the child at all.
    if (cancel && cancel->requested()) { res.cancelled = true; return res; }

    int fds[2];
    if (pipe(fds) != 0) return res;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return res;
    }
    if (pid == 0) {
#ifdef __linux__
        // Die with the parent. Without this a download outlives the app when it
        // is killed mid-transfer: curl keeps running detached, finishes writing
        // the .part file, and nobody is left to rename it into place.
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() == 1) _exit(1);   // parent already gone; lost the race
#endif
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);

        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);   // exec failed
    }
    close(fds[1]);

    // From here another thread can kill this child. adopt() fails when a cancel
    // landed in the window between the check above and the fork -- take it down
    // now rather than leave curl running with nobody waiting on it.
    if (cancel && !cancel->adopt(static_cast<std::intptr_t>(pid))) {
        ::kill(pid, SIGKILL);
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(
                              static_cast<long long>(timeout_sec * 1000));
    // Wait on the pipe rather than inside read(), so the deadline is honoured
    // whether or not the child ever says anything. Testing it after a read only
    // worked for a talkative child: a silent one -- ffmpeg on a clean decode,
    // curl stalled after connecting -- left this blocked in read() for as long
    // as it cared to run, and the timeout was never even looked at.
    char chunk[1024];
    bool timed_out = false;
    for (;;) {
        int wait_ms = -1;   // timeout_sec <= 0: block until output or exit
        if (timeout_sec > 0) {
            const auto left = deadline - std::chrono::steady_clock::now();
            wait_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(left).count());
            if (wait_ms <= 0) { timed_out = true; break; }
        }

        struct pollfd pfd = {fds[0], POLLIN, 0};
        const int n = poll(&pfd, 1, wait_ms);
        if (n == 0) { timed_out = true; break; }   // deadline, child still quiet
        if (n < 0) {
            if (errno == EINTR) continue;          // a signal, not a failure
            break;
        }

        // POLLHUP without POLLIN still needs the read: it returns 0 and ends
        // the loop, which is how a child that exits without output finishes.
        const ssize_t got = read(fds[0], chunk, sizeof(chunk));
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) break;                       // write end closed
        if (res.output.size() < kMaxCapturedOutput) {
            res.output.append(chunk, static_cast<std::size_t>(got));
        }
    }
    if (timed_out) kill(pid, SIGKILL);
    close(fds[0]);

    // Hand the child back before reaping it: once waitpid returns, the pid is
    // free to be reused, and a cancel arriving after that would hit a stranger.
    if (cancel) cancel->release();

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    // exec failure shows up as the 127 the child exited with.
    res.launched  = !(WIFEXITED(status) && WEXITSTATUS(status) == 127);
    res.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (cancel && cancel->requested()) res.cancelled = true;
    return res;
}

#endif

std::uint64_t size_on_disk(const paths::fs::path& p) {
    std::error_code ec;
    auto n = paths::fs::file_size(p, ec);
    return ec ? 0 : static_cast<std::uint64_t>(n);
}

}  // namespace

void Canceller::request() {
    std::lock_guard<std::mutex> lock(m_);
    requested_ = true;
    if (child_) kill_child(child_);
}

bool Canceller::requested() const {
    std::lock_guard<std::mutex> lock(m_);
    return requested_;
}

void Canceller::reset() {
    std::lock_guard<std::mutex> lock(m_);
    requested_ = false;
    child_ = 0;
}

bool Canceller::adopt(std::intptr_t child) {
    std::lock_guard<std::mutex> lock(m_);
    if (requested_) return false;
    child_ = child;
    return true;
}

void Canceller::release() {
    std::lock_guard<std::mutex> lock(m_);
    child_ = 0;
}

ProcResult run(const std::vector<std::string>& argv, double timeout_sec,
               Canceller* cancel) {
#ifdef _WIN32
    return run_win(argv, timeout_sec, cancel);
#else
    return run_posix(argv, timeout_sec, cancel);
#endif
}

bool can_download() {
    ProcResult r = run({"curl", "--version"}, 10);
    return r.launched && r.exit_code == 0;
}

DownloadResult download(const std::string& url, const paths::fs::path& dest,
                        std::uint64_t expected_bytes, const ProgressFn& progress,
                        Canceller* cancel) {
    DownloadResult out;

    std::error_code ec;
    if (dest.has_parent_path()) paths::fs::create_directories(dest.parent_path(), ec);

    const paths::fs::path tmp = dest.string() + ".part";
    paths::fs::remove(tmp, ec);

    // Poll the growing temp file so the UI can show a percentage.
    std::atomic<bool> done{false};
    std::thread watcher;
    if (progress) {
        watcher = std::thread([&] {
            while (!done.load()) {
                std::uint64_t have = size_on_disk(tmp);
                double frac = expected_bytes ? static_cast<double>(have) /
                                                   static_cast<double>(expected_bytes)
                                             : -1.0;
                if (frac > 1.0) frac = 1.0;
                progress(frac, have);
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
            }
        });
    }

    ProcResult r = run({"curl", "-fL", "--retry", "3", "--retry-delay", "2",
                        "--connect-timeout", "20", "-sS",
                        "-o", paths::to_utf8(tmp), url},
                       0, cancel);

    done.store(true);
    if (watcher.joinable()) watcher.join();

    // Checked before the failure branches below: a killed curl also looks like
    // a failed one, and being told a download you stopped "failed" is noise.
    if (r.cancelled) {
        paths::fs::remove(tmp, ec);
        out.cancelled = true;
        out.error = L("Download cancelled.", "İndirme iptal edildi.");
        return out;
    }
    if (!r.launched) {
        paths::fs::remove(tmp, ec);
        out.error = L("curl was not found — download the model by hand and point "
                      "Settings at it.",
                      "curl bulunamadı — modeli elle indirip Ayarlar'dan yolunu "
                      "gösterin.");
        return out;
    }
    if (r.exit_code != 0 || size_on_disk(tmp) == 0) {
        paths::fs::remove(tmp, ec);
        out.error = L("Download failed (", "İndirme başarısız (") + url + "): " +
                    (r.output.empty() ? ("curl exit " + std::to_string(r.exit_code))
                                      : r.output);
        return out;
    }

    paths::fs::remove(dest, ec);
    paths::fs::rename(tmp, dest, ec);
    if (ec) {
        out.error = L("The downloaded file could not be moved: ",
                      "İndirilen dosya taşınamadı: ") + ec.message();
        return out;
    }
    if (progress) progress(1.0, size_on_disk(dest));
    out.ok = true;
    return out;
}

}  // namespace transcriptor::net
