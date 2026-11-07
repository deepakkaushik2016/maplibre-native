#include <mln/tile/raster_dem_tile.hpp>

#include <mln/actor/scheduler.hpp>
#include <mln/renderer/buckets/hillshade_bucket.hpp>
#include <mln/renderer/tile_parameters.hpp>
#include <mln/renderer/tile_render_data.hpp>
#include <mln/storage/resource.hpp>
#include <mln/storage/response.hpp>
#include <mln/style/source.hpp>
#include <mln/tile/raster_dem_tile_worker.hpp>
#include <mln/tile/tile_loader_impl.hpp>
#include <mln/tile/tile_observer.hpp>
#include <utility>

namespace mln {

RasterDEMTile::RasterDEMTile(const OverscaledTileID& id_,
                             std::string sourceID_,
                             const TileParameters& parameters,
                             const Tileset& tileset,
                             TileObserver* observer_,
                             std::shared_ptr<KnownEmptyDEMTiles> knownEmpty_)
    : Tile(Kind::RasterDEM, id_, std::move(sourceID_), observer_),
      loader(*this, id_, parameters, tileset),
      threadPool(parameters.threadPool),
      mailbox(std::make_shared<Mailbox>(*Scheduler::GetCurrent())),
      worker(parameters.threadPool, ActorRef<RasterDEMTile>(*this, mailbox)),
      knownEmpty(std::move(knownEmpty_)) {
    if (knownEmpty && knownEmpty->contains(id.canonical)) {
        // Answered empty before: loaded, with nothing to draw, so the tile pyramid goes straight
        // to the ancestor that has data, and setNecessity asks the network for nothing.
        loaded = true;
    }
    encoding = tileset.rasterEncoding.value_or(Tileset::RasterEncoding::Mapbox);
    // A tile in the top row has no upper neighbours and one in the bottom row no lower ones, so
    // those count as backfilled. The z0 tile has neither, and its left and right neighbours are
    // itself in the next world copies, which add nothing to its border.
    const bool top = id.canonical.y == 0;
    const bool bottom = id.canonical.y + 1 == std::pow(2, id.canonical.z);
    if (top && bottom) {
        absentNeighbors = DEMTileNeighbors::Complete;
    } else if (top) {
        absentNeighbors = DEMTileNeighbors::NoUpper;
    } else if (bottom) {
        absentNeighbors = DEMTileNeighbors::NoLower;
    }
    neighboringTiles = absentNeighbors;
}

RasterDEMTile::~RasterDEMTile() {
    markObsolete();

    if (pending) {
        // This tile never finished loading or was abandoned, emit a cancellation event
        observer->onTileAction(id, sourceID, TileOperation::Cancelled);
    }

    // The bucket has resources that need to be released on the render thread.
    if (bucket) {
        threadPool.runOnRenderThread([bucket_{std::move(bucket)}] {});
    }
}

std::unique_ptr<TileRenderData> RasterDEMTile::createRenderData() {
    return std::make_unique<SharedBucketTileRenderData<HillshadeBucket>>(bucket);
}

void RasterDEMTile::setError(std::exception_ptr err) {
    loaded = true;
    observer->onTileError(*this, std::move(err));
}

void RasterDEMTile::setMetadata(std::optional<Timestamp> modified_, std::optional<Timestamp> expires_) {
    modified = std::move(modified_);
    expires = std::move(expires_);
}

void RasterDEMTile::setData(const std::shared_ptr<const std::string>& data) {
    if (!obsolete) {
        ++correlationID;

        if (!pending) {
            observer->onTileAction(id, sourceID, TileOperation::StartParse);
        }

        pending = true;
        worker.self().invoke(&RasterDEMTileWorker::parse, data, correlationID, encoding);
    }
}

void RasterDEMTile::onParsed(std::unique_ptr<HillshadeBucket> result, const uint64_t resultCorrelationID) {
    if (!obsolete) {
        if (knownEmpty) {
            if (result) {
                knownEmpty->remove(id.canonical);
            } else {
                // No data (204, or a 404): remembered until the answer expires (setMetadata).
                knownEmpty->add(id.canonical, expires);
            }
        }
        bucket = std::move(result);
        // The new DEM's border is a copy of its own edge again: no neighbour has backfilled it.
        neighboringTiles = absentNeighbors;
        loaded = true;
        if (resultCorrelationID == correlationID) {
            pending = false;
            observer->onTileAction(id, sourceID, TileOperation::EndParse);
        }
        renderable = static_cast<bool>(bucket);
        observer->onTileChanged(*this);
    }
}

void RasterDEMTile::onError(std::exception_ptr err, const uint64_t resultCorrelationID) {
    loaded = true;
    if (resultCorrelationID == correlationID) {
        pending = false;
        observer->onTileAction(id, sourceID, TileOperation::Error);
    }
    observer->onTileError(*this, std::move(err));
}

bool RasterDEMTile::layerPropertiesUpdated(const Immutable<style::LayerProperties>&) {
    return bool(bucket);
}

HillshadeBucket* RasterDEMTile::getBucket() const {
    return bucket.get();
}

bool RasterDEMTile::backfillBorder(const RasterDEMTile& borderTile, const DEMTileNeighbors mask) {
    int32_t dx = static_cast<int32_t>(borderTile.id.canonical.x) - static_cast<int32_t>(id.canonical.x);
    const auto dy = static_cast<int8_t>(static_cast<int32_t>(borderTile.id.canonical.y) -
                                        static_cast<int32_t>(id.canonical.y));
    const auto dim = static_cast<uint32_t>(pow(2, id.canonical.z));
    if (dx == 0 && dy == 0) return false;
    if (std::abs(dy) > 1) return false;
    // neighbor is in another world wrap
    if (std::abs(dx) > 1) {
        if (std::abs(int(dx + dim)) == 1) {
            dx += dim;
        } else if (std::abs(int(dx - dim)) == 1) {
            dx -= dim;
        }
    }
    const HillshadeBucket* borderBucket = borderTile.getBucket();
    if (!borderBucket || !bucket) {
        return false;
    }
    const DEMData& borderDEM = borderBucket->getDEMData();
    DEMData& tileDEM = bucket->getDEMData();

    const bool changed = tileDEM.backfillBorder(borderDEM, dx, dy);
    // update the bitmask to indicate that this tiles have been backfilled by flipping the relevant bit
    this->neighboringTiles = this->neighboringTiles | mask;
    if (changed) {
        // mark HillshadeBucket.prepared as false so it runs through the prepare
        // render pass with the new texture data we just backfilled
        bucket->setPrepared(false);
        bucket->renderTargetPrepared = false;
        // The color-relief layer samples the DEM texture directly, so its cached upload is
        // stale now too. (The terrain notices by the DEM's revision.)
        bucket->colorReliefDEMTexture.reset();
    }
    return changed;
}

void RasterDEMTile::setMask(TileMask&& mask) {
    if (bucket) {
        bucket->setMask(std::move(mask));
    }
}

void RasterDEMTile::setNecessity(TileNecessity newNecessity) {
    const bool revisit = newNecessity == TileNecessity::Required && necessity != TileNecessity::Required;
    necessity = newNecessity;
    // A loaded tile that answered empty is not asked for from the network again when it becomes
    // required (back in view, or out of the tile cache) while that answer holds: the loader would
    // otherwise request it on each such change. It is asked for from the cache, as such a request
    // first did before (MainResourceLoader answers from the database before the network), so data
    // an offline pack brought since is drawn: data drops the tile from the record, and the next
    // required tile revalidates it from the network as any other tile does.
    if (newNecessity == TileNecessity::Required && loaded && knownEmpty && knownEmpty->contains(id.canonical)) {
        if (revisit) {
            loader.revalidateFromCache();
        }
        return;
    }
    loader.setNecessity(newNecessity);
}

void RasterDEMTile::setUpdateParameters(const TileUpdateParameters& params) {
    loader.setUpdateParameters(params);
}

void RasterDEMTile::cancel() {
    markObsolete();
}

void RasterDEMTile::markObsolete() {
    obsolete = true;
    if (pending) {
        observer->onTileAction(id, sourceID, TileOperation::Cancelled);
    }
    pending = false;
    mailbox->abandon();
}

} // namespace mln
