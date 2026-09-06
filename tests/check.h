// The smallest thing that can fail a build. No framework: these tests exist to
// catch specific bugs coming back, and a dependency would cost more than it
// saves at this size.
#pragma once

#include <cstdio>
#include <string>

namespace test {

inline int g_failures = 0;

inline void check(const char* what, bool ok, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what,
                detail.empty() ? "" : "  --  ", detail.c_str());
    if (!ok) ++g_failures;
    std::fflush(stdout);
}

// Returns the process exit status: 0 when everything passed.
inline int summary(const char* suite) {
    if (g_failures == 0) {
        std::printf("\n%s: all checks passed\n", suite);
        return 0;
    }
    std::printf("\n%s: %d check(s) FAILED\n", suite, g_failures);
    return 1;
}

}  // namespace test
