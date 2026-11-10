#pragma once

#include <mln/tile/tile.hpp>
#include <mln/tile/tile_loader.hpp>
#include <mln/tile/raster_dem_tile_worker.hpp>
#include <mln/actor/actor.hpp>
#include <mln/storage/cache_generation.hpp>
#include <mln/util/chrono.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <unordered_map>

namespace mln {

class Tileset;
class TileParameters;
class HillshadeBucket;

enum class DEMTileNeighbors : uint8_t {
    // 0b00000000
    Empty = 0 << 1,

    // 0b00000001
    Left = 1 << 0,
    // 0b00000010
    Right = 1 << 1,
    // 0b00000100
    TopLeft = 1 << 2,
    // 0b00001000
    TopCenter = 1 << 3,
    // 0b00010000
    TopRight = 1 << 4,
    // 0b00100000
    BottomLeft = 1 << 5,
    // 0b01000000
    BottomCenter = 1 << 6,
    // 0b10000000
    BottomRight = 1 << 7,

    // helper enums for tiles with no upper/lower neighbors
    // and completely backfilled tiles

    // 0b00011100
    NoUpper = 0b00011100,
    // 0b11100000
    NoLower = 0b11100000,
    // 0b11111111
    Complete = 0b11111111
};

inline DEMTileNeighbors operator|(DEMTileNeighbors a, DEMTileNeighbors b) {
    return static_cast<DEMTileNeighbors>(int(a) | int(b));
};

inline DEMTileNeighbors operator&(DEMTileNeighbors a, DEMTileNeighbors b) {
    return static_cast<DEMTileNeighbors>(int(a) & int(b));
}

inline bool operator!=(DEMTileNeighbors a, DEMTileNeighbors b) {
    return static_cast<unsigned char>(a) != static_cast<unsigned char>(b);
}

namespace style {
class Layer;
} // namespace style

/// The tiles of one raster-dem source that answered empty this session: 204 No Content, or a 404,
/// at a zoom the source has. A sparse tileset has no finer data there, and the map draws the DEM
/// of the nearest ancestor with data instead (updateRenderables keeps it). A tile recorded here is
/// not asked for from the network again while the record holds: neither when the view comes back
/// to it, nor when the tile is created anew after leaving the tile cache. It is still asked for
/// from the cache (the ambient cache or offline database) then, as before, so data an offline pack
/// brought since is drawn.
///
/// The record holds until the answer's expiry (Cache-Control max-age or Expires), for the session
/// when it had none; an answer that may not be reused (no-store, no-cache, max-age=0) is not
/// recorded. Data arriving for the tile drops it, and the whole record lapses when the cache or
/// offline database changes as a whole (ambientCacheGeneration).
///
/// Looked up when a DEM tile is created and when one goes from optional to required, never per
/// frame. Bounded: past maxEntries the expired entries go, and if that is not enough the record
/// starts over, which costs at most one more request per tile; each entry is some 64 bytes with
/// the hash node, so 256 KB at most per source.
class KnownEmptyDEMTiles {
public:
    static constexpr std::size_t maxEntries = 4096;

    void add(const CanonicalTileID& id, std::optional<Timestamp> expires) {
        sync();
        const auto now = util::now();
        if (expires && *expires <= now) {
            // May not be reused: asked for again next time.
            if (entries.erase(id) > 0) {
                ++changes;
            }
            return;
        }
        if (entries.size() >= maxEntries && !entries.contains(id)) {
            std::erase_if(entries, [&](const auto& entry) { return entry.second && *entry.second <= now; });
            if (entries.size() >= maxEntries) {
                entries.clear();
            }
        }
        entries.insert_or_assign(id, expires);
        ++changes;
    }
    void remove(const CanonicalTileID& id) {
        if (entries.erase(id) > 0) {
            ++changes;
        }
    }
    void clear() {
        if (!entries.empty()) {
            ++changes;
        }
        entries.clear();
    }
    /// Moves whenever the record may answer contains() differently for some tile (an entry added,
    /// replaced or dropped, or the record lapsing as a whole), so a caller can keep what it
    /// derived from it until then. An entry reaching its expiry does not move it.
    uint64_t generation() {
        sync();
        return changes;
    }
    /// Whether `id` answered empty, that answer has not expired, and the cache has not changed as
    /// a whole since.
    bool contains(const CanonicalTileID& id) {
        sync();
        const auto it = entries.find(id);
        return it != entries.end() && (!it->second || *it->second > util::now());
    }
    std::size_t size() const { return entries.size(); }

private:
    /// Forgets everything when the cache changed as a whole since the last look.
    void sync() {
        const auto current = ambientCacheGeneration().load(std::memory_order_relaxed);
        if (current != cacheGeneration) {
            if (!entries.empty()) {
                ++changes;
            }
            entries.clear();
            cacheGeneration = current;
        }
    }

    std::unordered_map<CanonicalTileID, std::optional<Timestamp>> entries;
    uint64_t cacheGeneration = ambientCacheGeneration().load(std::memory_order_relaxed);
    uint64_t changes = 0;
};

class RasterDEMTile final : public Tile {
public:
    /// `knownEmpty`: the source's record of its tiles that answered empty (none when null). A tile
    /// in it starts out loaded with no data and asks for nothing until the record lapses.
    RasterDEMTile(const OverscaledTileID&,
                  std::string,
                  const TileParameters&,
                  const Tileset&,
                  TileObserver* observer = nullptr,
                  std::shared_ptr<KnownEmptyDEMTiles> knownEmpty = nullptr);
    ~RasterDEMTile() override;

    std::unique_ptr<TileRenderData> createRenderData() override;
    void setNecessity(TileNecessity) override;
    void setUpdateParameters(const TileUpdateParameters&) override;

    void setError(std::exception_ptr);
    void setMetadata(std::optional<Timestamp> modified, std::optional<Timestamp> expires);
    void setData(const std::shared_ptr<const std::string>& data);

    bool layerPropertiesUpdated(const Immutable<style::LayerProperties>& layerProperties) override;

    HillshadeBucket* getBucket() const;
    /// The bucket, shared: holding it keeps its DEM data alive across a reload of the tile.
    const std::shared_ptr<HillshadeBucket>& getSharedBucket() const { return bucket; }
    /// Backfills the border facing `borderTile` (at `mask`) from it; returns whether that changed
    /// the border.
    bool backfillBorder(const RasterDEMTile& borderTile, DEMTileNeighbors mask);

    // neighboringTiles is a bitmask for which neighboring tiles have been backfilled
    // there are max 8 possible neighboring tiles, so each bit represents one neighbor.
    // Reset to absentNeighbors whenever the tile's DEM is parsed anew.
    DEMTileNeighbors neighboringTiles = DEMTileNeighbors::Empty;
    /// The neighbours a tile in the top or bottom row of the world does not have.
    DEMTileNeighbors getAbsentNeighbors() const { return absentNeighbors; }

    void setMask(TileMask&&) override;

    void onParsed(std::unique_ptr<HillshadeBucket> result, uint64_t correlationID);
    void onError(std::exception_ptr, uint64_t correlationID);

    void cancel() override;

private:
    void markObsolete();

    TileLoader<RasterDEMTile> loader;

    TaggedScheduler threadPool;
    std::shared_ptr<Mailbox> mailbox;
    Actor<RasterDEMTileWorker> worker;

    uint64_t correlationID = 0;
    Tileset::RasterEncoding encoding;

    // Contains the Bucket object for the tile. Buckets are render
    // objects and they get added by tile parsing operations.
    std::shared_ptr<HillshadeBucket> bucket;

    bool obsolete = false;
    DEMTileNeighbors absentNeighbors = DEMTileNeighbors::Empty;

    std::shared_ptr<KnownEmptyDEMTiles> knownEmpty;
    /// The necessity last set, to tell a revisit (optional to required) from a repeat.
    TileNecessity necessity = TileNecessity::Optional;
};

} // namespace mln
