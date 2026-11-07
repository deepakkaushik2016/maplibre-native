#pragma once

#include <atomic>
#include <cstdint>

namespace mln {

/// A counter that moves whenever the ambient cache or the offline database changes as a whole: it
/// is invalidated, cleared, reset, moved, merged into, or an offline region is deleted, invalidated
/// or starts or stops downloading. State that the map derives from what the cache answered (the
/// raster-dem tiles known to be empty, KnownEmptyDEMTiles) compares it, and forgets that state when
/// it moved. Process-wide: the database file source and the renderer may run on different threads.
inline std::atomic<uint64_t>& ambientCacheGeneration() {
    static std::atomic<uint64_t> generation{0};
    return generation;
}

/// Moves ambientCacheGeneration.
inline void bumpAmbientCacheGeneration() {
    ambientCacheGeneration().fetch_add(1, std::memory_order_relaxed);
}

} // namespace mln
