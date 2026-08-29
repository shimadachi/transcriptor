// Which compute device the STT/LLM engines will use, and a label for the UI
// badge. Enumeration goes through ggml's backend registry, so CUDA, Metal,
// Vulkan and CPU are all discovered the same way.
#pragma once

#include <cstdint>
#include <string>

namespace transcriptor {

struct DeviceInfo {
    std::string device       = "cpu";   // "cuda" (any GPU backend) or "cpu"
    std::string compute_type = "int8";  // quantization hint, shown in the badge
    std::string name         = "CPU";   // human-readable
    std::string backend;                // "CUDA", "Metal", "Vulkan", "CPU"
    bool        gpu_available = false;
    int         gpu_count     = 0;
    std::uint64_t vram_total  = 0;      // bytes, 0 when unknown

    bool use_gpu() const { return device == "cuda"; }

    // e.g. "🟢 GPU · NVIDIA GeForce RTX 4060 · float16"
    std::string badge() const;
};

// prefer: "auto" | "cuda" | "cpu"
DeviceInfo resolve_device(const std::string& prefer = "auto",
                          const std::string& compute_type = "auto");

}  // namespace transcriptor
