// How many cores are actually worth handing to a compute-bound worker.
//
// std::thread::hardware_concurrency() counts *logical* processors, which on an
// SMT machine is twice the number that can do vector work at once. Sibling
// threads share the one core's vector units, so a saturated SIMD workload
// scheduled onto both of them contends with itself instead of going faster.
#pragma once

#include <string>

namespace transcriptor::util {

// The physical core count, or a conservative estimate when the platform will
// not say. Never returns 0. On a hybrid part, the performance cores only.
unsigned physical_cores();

#if !defined(_WIN32) && !defined(__APPLE__)
// The Linux query behind it, reading the sysfs tree under `sys_root` ("/sys"
// on a real system), so a test can hand it a hybrid machine. 0 when the tree
// says nothing usable.
unsigned physical_cores_in(const std::string& sys_root);
#endif

}  // namespace transcriptor::util
