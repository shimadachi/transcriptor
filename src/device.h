// Which compute device the STT/LLM engines will use, and a label for the UI
// badge. Enumeration goes through ggml's backend registry, so CUDA, Metal,
// Vulkan and CPU are all discovered the same way.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace transcriptor {

// One selectable accelerator, as ggml sees it.
struct ComputeDevice {
    std::string   id;            // ggml's device name, e.g. "Vulkan1" — what settings store
    std::string   name;          // description, as the backend reports it
    std::string   backend;       // "CUDA", "Metal", "Vulkan"
    bool          integrated = false;
    std::uint64_t vram_total  = 0;   // bytes, 0 when unknown

    // Index among the GPU-ish devices, in registry order. This is what whisper
    // counts with when it honours gpu_device, and it counts integrated GPUs
    // too -- which is how a laptop's iGPU ends up running the model while the
    // discrete card sits idle.
    int gpu_index = 0;
    // Index into the whole registry, for llama's explicit device list.
    int registry_index = 0;
};

// Every GPU (discrete and integrated) ggml can offer, in registry order.
std::vector<ComputeDevice> list_devices();

struct DeviceInfo {
    std::string device       = "cpu";   // "gpu" or "cpu"
    std::string compute_type = "int8";  // quantization hint, shown in the badge
    std::string name         = "CPU";   // human-readable
    std::string backend;                // "CUDA", "Metal", "Vulkan", "CPU"
    std::string id           = "cpu";   // the device actually chosen
    bool        gpu_available = false;
    bool        integrated    = false;
    int         gpu_count     = 0;
    std::uint64_t vram_total  = 0;      // bytes, 0 when unknown

    // Valid only when use_gpu(); see ComputeDevice above.
    int gpu_index      = 0;
    int registry_index = -1;

    bool use_gpu() const { return device == "gpu"; }

    // e.g. "🟢 GPU · <device name> · float16"
    std::string badge() const;
};

// Which of `devices` to run on for the setting `prefer`, or nullptr for the
// CPU. Kept free of ggml, so the rule can be tested without a GPU.
//
// "auto" -- and "cuda", its old spelling -- takes a discrete card ahead of an
// integrated one, since the integrated chip shares system memory and is the
// slower of the two whenever both exist. So does an id that is no longer here,
// which is what the settings save does with one as well: landing on the CPU
// instead left a GPU machine transcribing at a fraction of its speed because
// an id had changed -- "CUDA0" in the CUDA package is "Vulkan0" in the Vulkan
// one, and config.json goes from one to the other.
inline const ComputeDevice* choose_device(const std::vector<ComputeDevice>& devices,
                                          const std::string& prefer) {
    if (prefer == "cpu" || devices.empty()) return nullptr;
    for (const ComputeDevice& d : devices) {
        if (d.id == prefer) return &d;
    }
    for (const ComputeDevice& d : devices) {
        if (!d.integrated) return &d;
    }
    return &devices.front();
}

// prefer: "auto" | "cpu" | a ComputeDevice::id. "cuda" is accepted as the old
// spelling of "auto". An id that is no longer present falls back to "auto", so
// moving a drive between machines never leaves the app unable to start.
DeviceInfo resolve_device(const std::string& prefer = "auto",
                          const std::string& compute_type = "auto");

}  // namespace transcriptor
