#include "device.h"

#include <cstdlib>

#include <ggml-backend.h>

namespace transcriptor {

namespace {

struct GpuProbe {
    bool          found = false;
    int           count = 0;
    std::string   name;
    std::string   backend;
    std::uint64_t vram_total = 0;
};

// Walk ggml's registered devices and pick the first GPU. This does not create
// a compute context or allocate buffers — the app holds no VRAM until a model
// is actually loaded at transcription time.
GpuProbe probe_gpu() {
    GpuProbe p;

    // Respect an explicit CUDA_VISIBLE_DEVICES="" as a request for CPU.
    if (const char* v = std::getenv("CUDA_VISIBLE_DEVICES")) {
        std::string s(v);
        if (s.find_first_not_of(" \t") == std::string::npos) return p;
    }

    const size_t n = ggml_backend_dev_count();
    for (size_t i = 0; i < n; ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        if (!dev || ggml_backend_dev_type(dev) != GGML_BACKEND_DEVICE_TYPE_GPU) {
            continue;
        }
        ++p.count;
        if (p.found) continue;

        p.found = true;
        const char* desc = ggml_backend_dev_description(dev);
        p.name = desc ? desc : "GPU";

        ggml_backend_reg_t reg = ggml_backend_dev_backend_reg(dev);
        const char* rn = reg ? ggml_backend_reg_name(reg) : nullptr;
        p.backend = rn ? rn : "GPU";

        size_t free_bytes = 0, total_bytes = 0;
        ggml_backend_dev_memory(dev, &free_bytes, &total_bytes);
        p.vram_total = static_cast<std::uint64_t>(total_bytes);
    }
    return p;
}

}  // namespace

std::string DeviceInfo::badge() const {
    const char* icon = use_gpu() ? "🟢 GPU" : "🟡 CPU";
    return std::string(icon) + " · " + name + " · " + compute_type;
}

DeviceInfo resolve_device(const std::string& prefer,
                          const std::string& compute_type) {
    const GpuProbe gpu = probe_gpu();

    DeviceInfo info;
    info.gpu_available = gpu.found;
    info.gpu_count     = gpu.count;
    info.vram_total    = gpu.vram_total;

    const bool use_gpu = gpu.found && (prefer == "auto" || prefer == "cuda");
    if (use_gpu) {
        info.device       = "cuda";
        info.name         = gpu.name;
        info.backend      = gpu.backend;
        info.compute_type = (compute_type == "auto") ? "float16" : compute_type;
    } else {
        info.device       = "cpu";
        info.name         = "CPU";
        info.backend      = "CPU";
        info.compute_type = (compute_type == "auto") ? "int8" : compute_type;
    }
    return info;
}

}  // namespace transcriptor
