#include "device.h"

#include <cstdlib>

#include <ggml-backend.h>

namespace transcriptor {

namespace {

// An empty CUDA_VISIBLE_DEVICES is a request for CPU, and it is the one knob
// people reach for when a GPU has to be kept out of the way.
bool gpu_disabled_by_env() {
    if (const char* v = std::getenv("CUDA_VISIBLE_DEVICES")) {
        std::string s(v);
        if (s.find_first_not_of(" \t") == std::string::npos) return true;
    }
    return false;
}

}  // namespace

// Walking the registry does not create a context or allocate buffers — the app
// holds no VRAM until a model is actually loaded.
std::vector<ComputeDevice> list_devices() {
    std::vector<ComputeDevice> out;
    if (gpu_disabled_by_env()) return out;

    const size_t n = ggml_backend_dev_count();
    for (size_t i = 0; i < n; ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        if (!dev) continue;
        // `auto`: in C++ the function name hides the enum type of the same name.
        const auto type = ggml_backend_dev_type(dev);
        // IGPU belongs here as much as GPU does. Leaving it out was how an
        // integrated chip could run the model while the badge named the
        // discrete card: whisper counts both, this used to count one.
        if (type != GGML_BACKEND_DEVICE_TYPE_GPU &&
            type != GGML_BACKEND_DEVICE_TYPE_IGPU) {
            continue;
        }

        ComputeDevice d;
        d.gpu_index      = static_cast<int>(out.size());
        d.registry_index = static_cast<int>(i);
        d.integrated     = (type == GGML_BACKEND_DEVICE_TYPE_IGPU);

        const char* name = ggml_backend_dev_name(dev);
        d.id = name ? name : ("gpu" + std::to_string(d.gpu_index));
        const char* desc = ggml_backend_dev_description(dev);
        d.name = desc ? desc : d.id;

        ggml_backend_reg_t reg = ggml_backend_dev_backend_reg(dev);
        const char* rn = reg ? ggml_backend_reg_name(reg) : nullptr;
        d.backend = rn ? rn : "GPU";

        size_t free_bytes = 0, total_bytes = 0;
        ggml_backend_dev_memory(dev, &free_bytes, &total_bytes);
        d.vram_total = static_cast<std::uint64_t>(total_bytes);

        out.push_back(std::move(d));
    }
    return out;
}

std::string DeviceInfo::badge() const {
    const char* icon = use_gpu() ? "🟢 GPU" : "🟡 CPU";
    return std::string(icon) + " · " + name + " · " + compute_type;
}

DeviceInfo resolve_device(const std::string& prefer,
                          const std::string& compute_type) {
    const std::vector<ComputeDevice> devices = list_devices();

    DeviceInfo info;
    info.gpu_available = !devices.empty();
    info.gpu_count     = static_cast<int>(devices.size());

    const ComputeDevice* chosen = nullptr;
    if (prefer != "cpu" && !devices.empty()) {
        for (const ComputeDevice& d : devices) {
            if (d.id == prefer) { chosen = &d; break; }
        }
        // "auto" (and the older "cuda") pick for themselves: a discrete card
        // ahead of an integrated one, since the integrated chip shares system
        // memory and is the slower of the two whenever both exist.
        if (!chosen && (prefer == "auto" || prefer == "cuda" || prefer.empty())) {
            for (const ComputeDevice& d : devices) {
                if (!d.integrated) { chosen = &d; break; }
            }
            if (!chosen) chosen = &devices.front();
        }
    }

    if (chosen) {
        info.device         = "gpu";
        info.id             = chosen->id;
        info.name           = chosen->name;
        info.backend        = chosen->backend;
        info.integrated     = chosen->integrated;
        info.vram_total     = chosen->vram_total;
        info.gpu_index      = chosen->gpu_index;
        info.registry_index = chosen->registry_index;
        info.compute_type   = (compute_type == "auto") ? "float16" : compute_type;
    } else {
        info.device       = "cpu";
        info.id           = "cpu";
        info.name         = "CPU";
        info.backend      = "CPU";
        info.compute_type = (compute_type == "auto") ? "int8" : compute_type;
    }
    return info;
}

}  // namespace transcriptor
