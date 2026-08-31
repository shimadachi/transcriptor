#include "util/cpu.h"

#include <algorithm>
#include <cctype>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <vector>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#else
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#endif

namespace transcriptor::util {
namespace {

unsigned logical_cores() {
    const unsigned hw = std::thread::hardware_concurrency();
    return hw ? hw : 4;   // hardware_concurrency is allowed to answer 0
}

// Used whenever the platform query fails or returns something implausible.
// Halving assumes two-way SMT, which is the common case on x86; on a machine
// without it this under-counts, which costs some speed but never oversubscribes.
unsigned fallback() {
    return std::max(1u, logical_cores() / 2);
}

#if defined(_WIN32)
unsigned query() {
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
    if (bytes == 0) return 0;

    std::vector<char> buf(bytes);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &bytes)) return 0;

    // Variable-length records: each one is a core, so walk by Size and count.
    unsigned cores = 0;
    for (DWORD off = 0; off < bytes;) {
        auto* rec = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (rec->Size == 0) break;
        if (rec->Relationship == RelationProcessorCore) ++cores;
        off += rec->Size;
    }
    return cores;
}

#elif defined(__APPLE__)
unsigned query() {
    // Apple Silicon has no SMT but does have efficiency cores, which are much
    // slower; perflevel0 is the performance cluster. Intel Macs have no
    // perflevel keys at all, so fall through to the plain physical count.
    for (const char* key : {"hw.perflevel0.physicalcpu", "hw.physicalcpu"}) {
        int    value = 0;
        size_t size  = sizeof(value);
        if (sysctlbyname(key, &value, &size, nullptr, 0) == 0 && value > 0) {
            return static_cast<unsigned>(value);
        }
    }
    return 0;
}

#else
unsigned query() {
    // Linux exposes topology per CPU; a (package, core) pair identifies one
    // physical core, and its SMT siblings repeat the same pair.
    namespace fs = std::filesystem;

    const auto read_int = [](const fs::path& p, int* out) {
        std::ifstream f(p);
        return static_cast<bool>(f >> *out);
    };

    std::set<std::pair<int, int>> cores;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/devices/system/cpu", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("cpu", 0) != 0 || name.size() <= 3) continue;
        if (!std::all_of(name.begin() + 3, name.end(),
                         [](unsigned char c) { return std::isdigit(c); })) {
            continue;
        }

        const fs::path topo = entry.path() / "topology";
        int package = 0, core = 0;
        if (!read_int(topo / "physical_package_id", &package)) continue;
        if (!read_int(topo / "core_id", &core)) continue;
        cores.emplace(package, core);
    }
    return static_cast<unsigned>(cores.size());
}
#endif

}  // namespace

unsigned physical_cores() {
    const unsigned logical = logical_cores();
    const unsigned n       = query();

    // A count of zero means the query failed. One above the logical count means
    // it is answering a different question than we asked; either way, distrust
    // it rather than oversubscribe on the strength of a bad number.
    if (n == 0 || n > logical) return fallback();
    return n;
}

}  // namespace transcriptor::util
