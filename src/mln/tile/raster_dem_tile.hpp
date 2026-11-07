#pragma once

#include <mln/tile/tile.hpp>
#include <mln/tile/tile_loader.hpp>
#include <mln/tile/raster_dem_tile_worker.hpp>
#include <mln/actor/actor.hpp>
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
/// not asked for again while the record holds: neither when the view comes back to it, nor when
/// the tile is created anew after leaving the tile cache. The record holds until the answer's
/// expiry (its Cache-Control or Expires), for the session when it had none. Data arriving for the
/// tile drops it.
///
/// Looked up when a DEM tile is created and when one goes from optional to required, never per
/// frame. Bounded: past maxEntries the record starts over, which costs at most one more request
/// per tile; each entry is some 64 bytes with the hash node, so 256 KB at most per source.
class KnownEmptyDEMTiles {
public:
    static constexpr std::size_t maxEntries = 4096;

    void add(const CanonicalTileID& id, std::optional<Timestamp> expires) {
        if (entries.size() >= maxEntries && !entries.contains(id)) {
            entries.clear();
        }
        entries.insert_or_assign(id, expires);
    }
    void remove(const CanonicalTileID& id) { entries.erase(id); }
    void clear() { entries.clear(); }
    /// Whether `id` answered empty and that answer has not expired.
    bool contains(const CanonicalTileID& id) const {
        const auto it = entries.find(id);
        return it != entries.end() && (!it->second || *it->second > util::now());
    }
    std::size_t size() const { return entries.size(); }

private:
    std::unordered_map<CanonicalTileID, std::optional<Timestamp>> entries;
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
};

} // namespace mln
