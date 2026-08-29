// Web UI files compiled into the binary (see cmake/embed_assets.cmake).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace transcriptor {

struct Asset {
    const char* path;          // relative to web/, e.g. "index.html"
    const char* data;
    std::size_t size;
    const char* content_type;
};

// nullptr when `path` is not an embedded asset.
const Asset* find_asset(const std::string& path);

std::vector<const Asset*> all_assets();

}  // namespace transcriptor
