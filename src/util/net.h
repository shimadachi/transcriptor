// Minimal outbound networking: run a child process, and download a file.
//
// Downloads shell out to `curl`, which ships with Windows 10 1803+, every macOS
// and every mainstream Linux. That keeps a TLS stack (OpenSSL/schannel) out of
// the build while still allowing first-run model fetches. Nothing here touches
// recordings — only public model weights are ever transferred.
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "util/paths.h"

namespace transcriptor::net {

struct ProcResult {
    bool        launched = false;   // false = executable not found
    int         exit_code = -1;
    bool        cancelled = false;  // a Canceller stopped it; see below
    std::string output;             // stdout + stderr, capped at the first 8 KB
};

// Stops a run() or download() that another thread is blocked in.
//
// curl cannot be asked to stop politely, so cancelling kills the child outright
// and download() then deletes the half-written file rather than leaving one
// that looks complete. Cancelling before a call starts is allowed and makes it
// return without launching anything, which closes the window between a user
// pressing Cancel and the worker thread reaching the download.
class Canceller {
public:
    Canceller() = default;
    Canceller(const Canceller&) = delete;
    Canceller& operator=(const Canceller&) = delete;

    // Safe from any thread, and safe when nothing is running.
    void request();
    bool requested() const;

    // Clears the flag so the object can serve the next download. Only call it
    // when nothing is in flight.
    void reset();

    // -- for net.cpp itself ------------------------------------------------
    // Registers the child about to be waited on. False means a cancel landed
    // first, and the caller must not leave that child running.
    bool adopt(std::intptr_t child);
    // Forgets it. Must happen before the child is reaped: after that the OS is
    // free to reuse the id, and a late cancel would signal a stranger.
    void release();

private:
    mutable std::mutex m_;
    bool               requested_ = false;
    std::intptr_t      child_ = 0;
};

// Blocking. `timeout_sec` <= 0 means wait indefinitely.
ProcResult run(const std::vector<std::string>& argv, double timeout_sec = 0,
               Canceller* cancel = nullptr);

// True when a download tool is available.
bool can_download();

// fraction is in [0, 1], or -1 when the total size is unknown.
using ProgressFn = std::function<void(double fraction, std::uint64_t bytes)>;

struct DownloadResult {
    bool        ok = false;
    bool        cancelled = false;   // asked to stop; `error` says so plainly
    std::string error;
};

// Downloads to a temporary file next to `dest` and renames on success, so a
// partial transfer never leaves a corrupt model behind.
DownloadResult download(const std::string& url, const paths::fs::path& dest,
                        std::uint64_t expected_bytes = 0,
                        const ProgressFn& progress = nullptr,
                        Canceller* cancel = nullptr);

}  // namespace transcriptor::net
