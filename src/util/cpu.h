// How many cores are actually worth handing to a compute-bound worker.
//
// std::thread::hardware_concurrency() counts *logical* processors, which on an
// SMT machine is twice the number that can do vector work at once. Sibling
// threads share the one core's vector units, so a saturated SIMD workload
// scheduled onto both of them contends with itself instead of going faster.
#pragma once

namespace transcriptor::util {

// The physical core count, or a conservative estimate when the platform will
// not say. Never returns 0.
unsigned physical_cores();

}  // namespace transcriptor::util
