// Minimal outbound networking: run a child process, and download a file.
//
// Downloads shell out to `curl`, which ships with Windows 10 1803+, every macOS
// and every mainstream Linux. That keeps a TLS stack (OpenSSL/schannel) out of
// the build while still allowing first-run model fetches. Nothing here touches
// recordings — only public model weights are ever transferred.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "util/paths.h"

namespace transcriptor::net {

struct ProcResult {
    bool        launched = false;   // false = executable not found
    int         exit_code = -1;
    std::string output;             // stdout + stderr, trimmed to the tail
};

// Blocking. `timeout_sec` <= 0 means wait indefinitely.
ProcResult run(const std::vector<std::string>& argv, double timeout_sec = 0);

// True when a download tool is available.
bool can_download();

// fraction is in [0, 1], or -1 when the total size is unknown.
using ProgressFn = std::function<void(double fraction, std::uint64_t bytes)>;

struct DownloadResult {
    bool        ok = false;
    std::string error;
};

// Downloads to a temporary file next to `dest` and renames on success, so a
// partial transfer never leaves a corrupt model behind.
DownloadResult download(const std::string& url, const paths::fs::path& dest,
                        std::uint64_t expected_bytes = 0,
                        const ProgressFn& progress = nullptr);

}  // namespace transcriptor::net
