#pragma once

// URLs and data sources are read through the range-source add-on, by its C table, or without it
// through the basic input: a download into a temporary file. Both are a SourceReader.
#define RANGE_SOURCE_CLIENT_NAMESPACE webm_rs
#include "range_source.h"
#include "range_source_download.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace webm {

using SourceReader = webm_rs::Reader;
using SourceInfo = webm_rs::SourceInfo;
using Upstream = webm_rs::Upstream;
/** What a read waiting on the network checks to give up; the add-on's C table reads it too. */
using AbortFlag = std::atomic<int32_t>;

/**
 * `plugin` if this player can use it: the same ABI, and every function it calls. Otherwise null,
 * with `why` set.
 */
inline const RangeSourcePlugin* usablePlugin(const RangeSourcePlugin* plugin, std::string& why) {
    if (!plugin) return nullptr;
    if (plugin->abi != RANGE_SOURCE_ABI) {
        why = "the range-source add-on has ABI " + std::to_string(plugin->abi) + ", and this player needs " +
              std::to_string(RANGE_SOURCE_ABI);
        return nullptr;
    }
    if (plugin->structSize < offsetof(RangeSourcePlugin, releaseReader) + sizeof(plugin->releaseReader)) {
        why = "the range-source add-on is older than this player";
        return nullptr;
    }
    return plugin;
}

} // namespace webm
