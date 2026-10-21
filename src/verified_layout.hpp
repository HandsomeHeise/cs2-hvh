#pragma once
#include <cstdint>

namespace verified_layout {
// Engine level-name storage, decompiled for this profile:
// ev_ee79b58ce17b5a5e6060da9949cf5d10d8174b1840c7f309a7b588cf8ce017ef
// ev_eb8ed76289d715dbca34ea772aa401bc984074facfd5a301bdfa3a566a3631c4
inline constexpr std::uint32_t map_full = 0x210, map_short = 0x218;
// Confirmed against the local controller identity in the user's running CS2.
// analysis/live-data-validation.json; revalidated against a known entity on every lookup.
inline constexpr std::uint32_t identity_handle = 0x10, identity_stride = 0x70;
inline constexpr std::uint32_t chunk_table = 0x10, entries_per_chunk = 512, index_mask = 0x7fff;
}
