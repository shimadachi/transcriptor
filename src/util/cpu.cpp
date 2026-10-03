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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
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

    // Variable-length records: each one is a core, so walk by Size and count --
    // the fastest class only. A hybrid part reports its efficiency cores with
    // a lower EfficiencyClass, and they are much slower, as on Apple Silicon
    // below; a part with one kind of core reports 0 for all of them.
    unsigned cores = 0;
    BYTE best = 0;
    for (DWORD off = 0; off < bytes;) {
        auto* rec = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (rec->Size == 0) break;
        if (rec->Relationship == RelationProcessorCore) {
            const BYTE cls = rec->Processor.EfficiencyClass;
            if (cls > best) { best = cls; cores = 0; }
            if (cls == best) ++cores;
        }
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
unsigned query() { return physical_cores_in("/sys"); }
#endif

}  // namespace

#if !defined(_WIN32) && !defined(__APPLE__)
namespace {

// "0-7,16,18-19" -> {0..7, 16, 18, 19}: the kernel's cpulist format.
std::set<int> cpu_list(const std::string& text) {
    std::set<int> out;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t comma = text.find(',', at);
        const std::string part = text.substr(at, comma == std::string::npos
                                                     ? std::string::npos : comma - at);
        const std::size_t dash = part.find('-');
        try {
            const int lo = std::stoi(part.substr(0, dash));
            const int hi = dash == std::string::npos ? lo : std::stoi(part.substr(dash + 1));
            for (int c = lo; c <= hi && c - lo < 4096; ++c) out.insert(c);
        } catch (const std::exception&) {
            // A part that is not a number contributes nothing.
        }
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    return out;
}

}  // namespace

unsigned physical_cores_in(const std::string& sys_root) {
    // Linux exposes topology per CPU; a (package, core) pair identifies one
    // physical core, and its SMT siblings repeat the same pair.
    namespace fs = std::filesystem;
    const fs::path sys(sys_root);

    const auto read_int = [](const fs::path& p, int* out) {
        std::ifstream f(p);
        return static_cast<bool>(f >> *out);
    };

    // A hybrid Intel part lists its performance cores' CPUs here, and its
    // efficiency cores' under cpu_atom. Only the first kind is counted: the
    // efficiency cores are much slower, and a compute-bound pool split evenly
    // across both waits on them -- the reason the Apple branch counts its
    // performance cluster alone, and llama.cpp leaves them out on x86 too.
    std::set<int> performance;
    {
        std::ifstream f(sys / "devices" / "cpu_core" / "cpus");
        std::string text;
        if (std::getline(f, text)) performance = cpu_list(text);
    }

    std::set<std::pair<int, int>> cores;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(sys / "devices" / "system" / "cpu", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("cpu", 0) != 0 || name.size() <= 3) continue;
        if (!std::all_of(name.begin() + 3, name.end(),
                         [](unsigned char c) { return std::isdigit(c); })) {
            continue;
        }
        if (!performance.empty() &&
            !performance.count(static_cast<int>(std::strtol(name.c_str() + 3, nullptr, 10)))) {
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
