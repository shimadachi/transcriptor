// Regression test for choosing the compute device (V22).
//
// The bug: a device id that was no longer present -- the card unplugged, or
// config.json carried from the CUDA package to the Vulkan one, where "CUDA0" is
// "Vulkan0" -- sent the app to the CPU at startup, although the header promised
// it would fall back to choosing automatically, and a settings save did exactly
// that. A GPU machine transcribed at a fraction of its speed with nothing but
// the badge to say so. choose_device() is ggml-free, so no GPU is needed here.

#include <string>
#include <vector>

#include "check.h"
#include "device.h"

using transcriptor::choose_device;
using transcriptor::ComputeDevice;

namespace {

ComputeDevice card(const std::string& id, bool integrated) {
    ComputeDevice d;
    d.id = id;
    d.integrated = integrated;
    return d;
}

std::string id_of(const ComputeDevice* d) { return d ? d->id : "cpu"; }

}  // namespace

int main() {
    const std::vector<ComputeDevice> laptop = {card("Vulkan0", true), card("Vulkan1", false)};

    test::check("V22 a card that is no longer here falls back to automatic",
                id_of(choose_device(laptop, "CUDA0")) == "Vulkan1",
                id_of(choose_device(laptop, "CUDA0")));
    test::check("automatic prefers the discrete card",
                id_of(choose_device(laptop, "auto")) == "Vulkan1");
    test::check("the old \"cuda\" spelling still means automatic",
                id_of(choose_device(laptop, "cuda")) == "Vulkan1");
    test::check("a card that is here is honoured, integrated or not",
                id_of(choose_device(laptop, "Vulkan0")) == "Vulkan0");
    test::check("\"cpu\" means the CPU", choose_device(laptop, "cpu") == nullptr);
    test::check("with no card at all, everything is the CPU",
                choose_device({}, "Vulkan1") == nullptr);
    const std::vector<ComputeDevice> igpu_only = {card("Metal", true)};
    test::check("an integrated chip is used when it is all there is",
                id_of(choose_device(igpu_only, "gone")) == "Metal");
    return test::summary("device choice");
}
