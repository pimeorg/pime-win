// Per-user key that encrypts pime-core's plugin KV store.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pimewin {

// Loads <data_dir>\store.key (DPAPI, 32 bytes), creating it on first use.
// Returns false when the key cannot be read or created; callers should keep
// running without a store instead of silently replacing a damaged key.
bool LoadOrCreateStoreKey(const std::wstring& data_dir, std::vector<uint8_t>* key);

}  // namespace pimewin