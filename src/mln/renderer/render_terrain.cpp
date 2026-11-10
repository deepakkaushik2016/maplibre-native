#include <mln/renderer/render_terrain.hpp>
#include <mln/renderer/update_parameters.hpp>
#include <mln/renderer/render_source.hpp>
#include <mln/renderer/render_tile.hpp>
#include <mln/renderer/render_pass.hpp>
#include <mln/renderer/render_tree.hpp>
#include <mln/renderer/render_static_data.hpp>
#include <mln/renderer/render_orchestrator.hpp>
#include <mln/renderer/render_target.hpp>
#include <mln/renderer/dem_elevation_provider.hpp>
#include <mln/renderer/paint_parameters.hpp>
#include <mln/renderer/change_request.hpp>
#include <mln/renderer/layer_group.hpp>
#include <mln/renderer/layers/terrain_layer_tweaker.hpp>
#include <mln/renderer/buckets/hillshade_bucket.hpp>
#include <mln/geometry/dem_data.hpp>
#include <mln/util/tile_cover.hpp>
#include <mln/tile/raster_dem_tile.hpp>
#include <mln/tile/tile.hpp>
#if MLN_RENDER_BACKEND_OPENGL
#include <mln/gl/context.hpp>
#include <mln/gl/texture_2d_array.hpp>
#include <mln/gl/drawable_gl.hpp> // gl::DrawableGL::setArrayTexture for the instanced depth DEM
#endif
#include <mln/gfx/context.hpp>
#include <mln/gfx/renderable.hpp>
#include <mln/gfx/renderer_backend.hpp>
#include <mln/gfx/drawable.hpp>
#include <mln/gfx/drawable_impl.hpp>
#include <mln/gfx/drawable_builder.hpp>
#include <mln/gfx/shader_registry.hpp>
#include <mln/gfx/color_mode.hpp>
#include <mln/gfx/texture2d.hpp>
#include <mln/shaders/shader_source.hpp>
#include <mln/shaders/terrain_layer_ubo.hpp>
#include <mln/shaders/shader_defines.hpp>
#include <mln/shaders/segment.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/geo.hpp>
#include <mln/util/projection.hpp>
#include <mln/math/angles.hpp>
#include <mln/util/logging.hpp>
#include <mln/util/image.hpp>
#include <mln/util/mat4.hpp>
#include <mln/util/monotonic_timer.hpp>
#include <mln/map/transform_state.hpp>
#include <mln/map/camera.hpp>
#include <mln/util/convert.hpp>         // util::cast for the instanced depth UBO matrix
#include <mln/util/hash.hpp>            // util::hash_combine for the depth-instance set signature
#include <mln/gfx/vertex_attribute.hpp> // VertexAttributeArray for the a_instance attribute

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mln {

namespace {

// Scale and x/y offset mapping a child tile's local space into the (possibly
// ancestor) DEM tile that covers it: a child dz levels deeper occupies the
// 1/scale sub-square at (dx, dy) of the ancestor.
struct DEMSubTileOffset {
    float scale;
    float dx;
    float dy;
};

// Squared distance from `origin` (normalized web-mercator) to the tile's centre.
double tileCentreDistance2(const UnwrappedTileID& id, const std::array<double, 2>& origin) {
    const double scale = static_cast<double>(1u << id.canonical.z);
    const double dx = (static_cast<double>(id.canonical.x) + 0.5) / scale + id.wrap - origin[0];
    const double dy = (static_cast<double>(id.canonical.y) + 0.5) / scale - origin[1];
    return dx * dx + dy * dy;
}

DEMSubTileOffset demSubTileOffset(const CanonicalTileID& child, const CanonicalTileID& ancestor) {
    const int dz = child.z - ancestor.z;
    return {.scale = static_cast<float>(1u << dz),
            .dx = static_cast<float>(child.x - (ancestor.x << dz)),
            .dy = static_cast<float>(child.y - (ancestor.y << dz))};
}

// The DEM data a render tile holds, or null when it has none to sample yet. A render tile
// outlives the frame that built it: its tile can be reloaded meanwhile (with no content, say),
// which drops the bucket until the next frame drops the render tile.
const DEMData* demDataOf(const RenderTile& renderTile) {
    const auto& tile = renderTile.getTile();
    if (tile.kind != Tile::Kind::RasterDEM) {
        return nullptr;
    }
    const auto* bucket = static_cast<const RasterDEMTile&>(tile).getBucket();
    if (!bucket) {
        return nullptr;
    }
    const auto& demData = bucket->getDEMData();
    return demData.getImagePtr() && demData.dim > 0 ? &demData : nullptr;
}

// demDataOf, holding the bucket that owns the data.
std::shared_ptr<const DEMData> sharedDEMDataOf(const RenderTile& renderTile) {
    if (!demDataOf(renderTile)) {
        return nullptr;
    }
    const auto& bucket = static_cast<const RasterDEMTile&>(renderTile.getTile()).getSharedBucket();
    return {bucket, &bucket->getDEMData()};
}

// The DEM height (not exaggerated) at a coordinate local to a tile the DEM tile covers, sampled as
// the shaders' get_elevation() samples the DEM texture: DEM pixel i describes the cell centred at
// tile coordinate (i + 0.5) / dim, and the height is bilinear between those centres, with the
// pixel indices clamped to the data plus its 2-texel border (backfilled from the neighbouring DEM
// tiles), not to the data alone. Within half a pixel of a DEM tile's edge the height thus runs on
// into the neighbour, as maplibre-gl-js Terrain.getDEMElevation reads it
// (`DEM_CELL_CENTER_OFFSET`).
float sampleDEM(const DEMData& dem, const DEMSubTileOffset& off, float x, float y) {
    constexpr float cellCentre = 0.5f;
    const float dim = static_cast<float>(dem.dim);
    const float px = (off.dx * util::EXTENT + x) / off.scale / util::EXTENT * dim - cellCentre;
    const float py = (off.dy * util::EXTENT + y) / off.scale / util::EXTENT * dim - cellCentre;
    const float x0 = std::floor(px);
    const float y0 = std::floor(py);
    const float fx = px - x0;
    const float fy = py - y0;
    const auto texel = [&](float t) {
        return static_cast<int32_t>(util::clamp(t, -2.0f, dim + 1.0f));
    };
    const int32_t xa = texel(x0);
    const int32_t xb = texel(x0 + 1.0f);
    const int32_t ya = texel(y0);
    const int32_t yb = texel(y0 + 1.0f);
    const float tl = dem.get(xa, ya);
    const float tr = dem.get(xb, ya);
    const float bl = dem.get(xa, yb);
    const float br = dem.get(xb, yb);
    const float top = tl + (tr - tl) * fx;
    const float bottom = bl + (br - bl) * fx;
    return top + (bottom - top) * fy;
}

// The texel holding `meters` in a DEM encoding, to the encoding's step (0.1 m for Mapbox, 1/256 m
// for Terrarium). The shader decodes dot(rgb * 255, unpack.xyz) - unpack.w, and in both encodings
// R, G and B are the base-256 digits of (meters + unpack.w) / unpack.z. At 0 m that is
// (1, 134, 160) for Mapbox and (128, 0, 0) for Terrarium; an all-zero texel would instead be
// -unpack.w (-10 km, -32 km), which sinks whatever samples it out of view.
std::array<uint8_t, 4> elevationTexel(const std::array<float, 4>& unpack, double meters) {
    const double steps = std::round((meters + unpack[3]) / unpack[2]);
    const auto value = static_cast<uint32_t>(std::clamp(steps, 0.0, double{0xFFFFFF}));
    return {{static_cast<uint8_t>(value >> 16),
             static_cast<uint8_t>((value >> 8) & 0xFF),
             static_cast<uint8_t>(value & 0xFF),
             255}};
}

// Where a stitched pixel's centre falls in a descendant `ratio` times finer, along one axis: the
// two descendant pixels either side of it (clamped to the data plus its 2-pixel border) and the
// weight of the second. `t` is the stitched pixel's offset from the descendant's origin.
struct ResampleTap {
    int32_t first;
    int32_t second;
    double weight;
};
ResampleTap resampleTap(int32_t t, double ratio, int32_t dim) {
    // DEM pixel i describes the cell centred at i + 0.5, in both the stitched DEM and the descendant.
    const double p = (t + 0.5) * ratio - 0.5;
    const double i = std::floor(p);
    const auto first = static_cast<int32_t>(i);
    return {.first = util::clamp(first, -2, dim + 1), .second = util::clamp(first + 1, -2, dim + 1), .weight = p - i};
}

} // namespace

std::shared_ptr<const DEMData> RenderTerrain::stitchDEM(const CanonicalTileID& tile,
                                                        const std::vector<StitchSource>& sources) {
    const DEMData& first = *sources.front().dem;
    const int32_t dim = first.dim * 2;
    const int32_t stride = dim + 4;
    auto image = std::make_shared<PremultipliedImage>(
        Size(static_cast<uint32_t>(stride), static_cast<uint32_t>(stride)));
    auto* out = reinterpret_cast<uint32_t*>(image->data.get());
    const auto& unpack = first.getUnpackVector();
    const auto zero = elevationTexel(unpack, 0.0);
    uint32_t zeroBits = 0;
    std::memcpy(&zeroBits, zero.data(), sizeof(zeroBits));
    std::fill(out, out + static_cast<size_t>(stride) * stride, zeroBits);

    for (const auto& source : sources) {
        const DEMData& dem = *source.dem;
        const int dz = source.id.z - tile.z;
        // One source, one tile size; a descendant too deep to span a stitched texel adds nothing.
        if (dem.dim != first.dim || dz <= 0 || dz >= 31 || (dim >> dz) == 0) {
            continue;
        }
        const int32_t size = dim >> dz;
        const auto x0 = static_cast<int32_t>((static_cast<int64_t>(source.id.x) - (int64_t{tile.x} << dz)) * size);
        const auto y0 = static_cast<int32_t>((static_cast<int64_t>(source.id.y) - (int64_t{tile.y} << dz)) * size);
        const int32_t xa = x0 == 0 ? -2 : x0;
        const int32_t xb = x0 + size == dim ? dim + 2 : x0 + size;
        const int32_t ya = y0 == 0 ? -2 : y0;
        const int32_t yb = y0 + size == dim ? dim + 2 : y0 + size;
        const auto* in = reinterpret_cast<const uint32_t*>(dem.getImage()->data.get());
        const auto inRow = [&](int32_t y) {
            return in + static_cast<ptrdiff_t>(y + 2) * dem.stride + 2;
        };
        if (size == dem.dim) {
            // A child: pixel for pixel (t may run into the border).
            for (int32_t y = ya; y < yb; ++y) {
                uint32_t* row = out + static_cast<ptrdiff_t>(y + 2) * stride + 2;
                std::memcpy(row + xa, inRow(y - y0) + (xa - x0), static_cast<size_t>(xb - xa) * sizeof(uint32_t));
            }
            continue;
        }
        // A deeper descendant: each stitched pixel holds the descendant's surface at its centre,
        // bilinear between the descendant pixels around it, as get_elevation() reads that
        // descendant there. Decoded and encoded again, since packed heights do not interpolate.
        const double ratio = static_cast<double>(dem.dim) / size;
        std::vector<ResampleTap> columns;
        columns.reserve(static_cast<size_t>(xb - xa));
        for (int32_t x = xa; x < xb; ++x) {
            columns.push_back(resampleTap(x - x0, ratio, dem.dim));
        }
        for (int32_t y = ya; y < yb; ++y) {
            const ResampleTap rowTap = resampleTap(y - y0, ratio, dem.dim);
            uint32_t* row = out + static_cast<ptrdiff_t>(y + 2) * stride + 2;
            for (int32_t x = xa; x < xb; ++x) {
                const ResampleTap& col = columns[static_cast<size_t>(x - xa)];
                const double t0 = dem.get(col.first, rowTap.first);
                const double t1 = dem.get(col.second, rowTap.first);
                const double b0 = dem.get(col.first, rowTap.second);
                const double b1 = dem.get(col.second, rowTap.second);
                const double upper = t0 + (t1 - t0) * col.weight;
                const double lower = b0 + (b1 - b0) * col.weight;
                const auto texel = elevationTexel(unpack, upper + (lower - upper) * rowTap.weight);
                std::memcpy(&row[x], texel.data(), sizeof(uint32_t));
            }
        }
    }
    return std::make_shared<const DEMData>(std::move(image), first.encoding);
}

RenderTerrain::RenderTerrain(Immutable<style::Terrain::Impl> impl_)
    : impl(std::move(impl_)) {}

RenderTerrain::~RenderTerrain() = default;

std::set<UnwrappedTileID> RenderTerrain::computeMeshCover(
    const TransformState& state, const std::shared_ptr<UpdateParameters>& updateParameters) const {
    std::set<UnwrappedTileID> out;
    if (!demSource) {
        return out;
    }

    // Elevation-aware visibility: terrain leaning towards the camera occupies
    // screen space its flat footprint does not, so the cover is tested against the
    // DEM height rather than the z=0 plane (as util::tileCover / gl-js do).
    DEMElevationProvider elevationProvider(demSource, getExaggeration());

    // Cover at the tile size the DEM source selects tiles with, so the mesh lands on the zoom its
    // DEM tiles are on. Meshing shallower than the DEM leaves its tiles descendants, which the
    // per-tile lookup cannot match, so the mesh renders flat off the placeholder; it also
    // undersamples the DEM, aliasing the relief into waves on the fixed 128x128 mesh. Still the
    // elevation-aware ideal cover from the view, not the DEM's loaded tile set.
    const uint16_t terrainCoverTileSize = demSource->getTileSize();
    // Deeper than the DEM goes, as maplibre-gl-js does: a tile past its maxzoom samples its DEM
    // ancestor through a sub-tile offset (the ancestor fallback in update()), and its 1024 px
    // drape target covers a smaller area. Capped at the DEM's maxzoom, a close-up at z16 over a
    // z12 DEM stretched each target 8x (dotted roads, blocky imagery). How deep depends on the
    // load mode (meshMaxZoom): Quality follows the view as gl-js does; Balanced and Performance
    // stop sooner, since each level deeper adds tiles, and drape memory with them.
    const TerrainLoadMode loadMode = updateParameters ? updateParameters->terrainLoadMode : TerrainLoadMode::Quality;
    const Range<uint8_t> zoomRange{0, meshMaxZoom(loadMode, demSource->getMaxZoom())};

    // LOD parameters from the frame drive the same near-high/far-low zoom
    // selection every other source uses, so the near field drapes at a higher
    // zoom (smaller ground area per 1024 target = sharper draped content).
    util::TileCoverParameters coverParams{.transformState = state, .elevationProvider = &elevationProvider};
    // The finest tiles go under the camera (where the screen is densest), as the mesh-tile cap below
    // keeps the tiles nearest it; measured from the map centre, a steep pitch gives the ground at the
    // bottom of the screen the coarsest tiles of the cover.
    coverParams.tileLodOrigin = meshTileCapOrigin(state);
    double zoomShift = 0.0;
    if (updateParameters) {
        coverParams.tileLodMinRadius = updateParameters->tileLodMinRadius;
        coverParams.tileLodScale = updateParameters->tileLodScale;
        coverParams.tileLodPitchThreshold = updateParameters->tileLodPitchThreshold;
        coverParams.tileLodMode = updateParameters->tileLodMode;
        zoomShift = updateParameters->tileLodZoomShift;
    }

    const double zoom = util::clamp<double>(state.getZoom() + zoomShift, state.getMinZoom(), state.getMaxZoom());
    const int32_t overscaledZoom = util::coveringZoomLevel(zoom, style::SourceType::RasterDEM, terrainCoverTileSize);
    if (std::cmp_less(overscaledZoom, zoomRange.min)) {
        return out;
    }
    const int32_t idealZoom = std::min<int32_t>(zoomRange.max, overscaledZoom);
    for (const auto& id : util::tileCover(
             coverParams, static_cast<uint8_t>(idealZoom), zoomRange, static_cast<uint8_t>(overscaledZoom))) {
        out.insert(id.toUnwrapped());
    }

    // One-ring cover dilation. tileCover is a top-down DFS that only descends into tiles
    // whose (sea-level) ancestor intersects the frustum, so a frontier tile whose terrain
    // rises into view but whose flat ancestor was culled is never even visited - it stays
    // out of the cover, and the terrain draws a skirt with nothing behind it, until a
    // camera nudge shifts the frustum and pulls it in. Add every 8-neighbour of the cover,
    // then keep only those whose elevation-extended bounds still intersect the frustum
    // (frustumCull, using the DEM provider's conservative fallback range for the
    // not-yet-loaded neighbours). The elevation sign decides which neighbours survive, so
    // this is correct for terrain above OR below sea level (bathymetry) without hardcoding
    // a direction; the frustum trim stops it from tripling the cover like a blind ring
    // would. Drop the frustumCull line to fall back to a pure uniform 1-ring dilation.
    std::set<UnwrappedTileID> dilated = dilateCover(out);
    if (dilated.size() != out.size()) {
        out = util::frustumCull(coverParams, dilated);
    }

    // Cap the tile count: keep those nearest the camera, drop the farthest (the horizon tiles a
    // high tilt pulls in). Nearest the camera, not the map centre: at a steep tilt the centre is
    // far ahead, and the tiles right under the camera - the bottom corners of the screen, the
    // biggest on it - would rank as far and be dropped, showing the skirts of the tiles behind
    // them as streaks. Per-mode cap (TerrainLoadBudget::maxMeshTiles): Quality keeps
    // a generous cap so terrain render distance stays long; Balanced/Performance trade distance
    // for frame time. It caps the cover, not just the mesh: the drape-target pool allocates a
    // 1024 px target per cover tile, so a cap on the mesh alone still allocated one for every
    // tile it dropped.
    // Before the cap drops any tile, the farthest tiles past the DEM's maxzoom go back to their
    // parents (coarsenFarMeshTiles): the deeper mesh (meshMaxZoom) then costs detail in the
    // distance, where a tile covers little of the screen, instead of terrain render distance.
    // Never shallower than the DEM, which the per-tile lookup needs (see above), so the cover is
    // never coarser than it would be without the deeper mesh.
    if (updateParameters) {
        const size_t maxTiles = terrainLoadBudget(updateParameters->terrainLoadMode).maxMeshTiles;
        const auto origin = meshTileCapOrigin(state);
        out = coarsenFarMeshTiles(std::move(out), maxTiles, demSource->getMaxZoom(), origin);
        out = nearestMeshTiles(std::move(out), maxTiles, origin);
    }
    return out;
}

std::array<float, 3> RenderTerrain::demTileMapping(const CanonicalTileID& tile, const CanonicalTileID& demTile) {
    const auto off = demSubTileOffset(tile, demTile);
    return {{1.0f / (util::EXTENT * off.scale), off.dx / off.scale, off.dy / off.scale}};
}

std::set<UnwrappedTileID> RenderTerrain::dilateCover(const std::set<UnwrappedTileID>& cover) {
    std::set<UnwrappedTileID> dilated = cover;
    // Every tile kept and every ancestor of one: a neighbour in this set lies over a tile kept.
    std::set<UnwrappedTileID> overlapped;
    const auto keep = [&](const UnwrappedTileID& id) {
        for (int z = id.canonical.z; z >= 0; --z) {
            overlapped.emplace(id.wrap, id.canonical.scaledTo(static_cast<uint8_t>(z)));
        }
    };
    for (const auto& id : cover) keep(id);
    // Whether a tile kept lies over this tile (an ancestor of it), or under it.
    const auto coveredAlready = [&](const UnwrappedTileID& tile) {
        if (overlapped.contains(tile)) return true;
        for (int z = tile.canonical.z - 1; z >= 0; --z) {
            if (dilated.contains(UnwrappedTileID(tile.wrap, tile.canonical.scaledTo(static_cast<uint8_t>(z))))) {
                return true;
            }
        }
        return false;
    };

    for (const auto& id : cover) {
        const int32_t numTiles = 1 << id.canonical.z;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const int ny = static_cast<int>(id.canonical.y) + dy;
                if (ny < 0 || ny >= numTiles) continue; // nothing past the poles
                int nx = static_cast<int>(id.canonical.x) + dx;
                int nwrap = id.wrap;
                if (nx < 0) {
                    nx += numTiles;
                    --nwrap;
                } else if (nx >= numTiles) {
                    nx -= numTiles;
                    ++nwrap;
                }
                const UnwrappedTileID neighbour(
                    static_cast<int16_t>(nwrap),
                    CanonicalTileID(id.canonical.z, static_cast<uint32_t>(nx), static_cast<uint32_t>(ny)));
                // The neighbour at this tile's zoom may sit over ground a tile of another zoom
                // already covers (the cover mixes zooms, a finer tile beside a coarser one): meshing
                // it would add a second surface over that ground, sampled from a coarser or finer
                // DEM, and a drape target and 1024 px of texture to hold it. Only ground the cover
                // leaves out is worth a tile.
                if (coveredAlready(neighbour)) continue;
                dilated.insert(neighbour);
                keep(neighbour);
            }
        }
    }
    return dilated;
}

std::array<double, 2> RenderTerrain::meshTileCapOrigin(const TransformState& state) {
    // The ground under the camera in normalized web-mercator [0,1] (standard projection): the
    // map centre, moved back along the view by the camera's horizontal distance from it.
    const LatLng center = state.getLatLng();
    const double latRad = util::deg2rad(center.latitude());
    const double back = state.getCameraToCenterDistance() * std::sin(state.getPitch()) /
                        Projection::worldSize(state.getScale());
    const double compass = -state.getBearing(); // radians, clockwise from north
    return {center.longitude() / 360.0 + 0.5 - back * std::sin(compass),
            0.5 - std::log(std::tan(M_PI / 4.0 + latRad / 2.0)) / (2.0 * M_PI) + back * std::cos(compass)};
}

std::set<UnwrappedTileID> RenderTerrain::nearestMeshTiles(std::set<UnwrappedTileID> tiles,
                                                          size_t maxTiles,
                                                          const std::array<double, 2>& origin) {
    if (maxTiles == 0 || tiles.size() <= maxTiles) {
        return tiles;
    }

    // Ties go by tile ID, so the same view keeps the same tiles from one call to the next.
    std::vector<UnwrappedTileID> sorted(tiles.begin(), tiles.end());
    std::partial_sort(sorted.begin(),
                      sorted.begin() + static_cast<std::ptrdiff_t>(maxTiles),
                      sorted.end(),
                      [&](const UnwrappedTileID& a, const UnwrappedTileID& b) {
                          const double da = tileCentreDistance2(a, origin);
                          const double db = tileCentreDistance2(b, origin);
                          return da < db || (da == db && a < b);
                      });
    return {sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(maxTiles)};
}

std::set<UnwrappedTileID> RenderTerrain::coarsenFarMeshTiles(std::set<UnwrappedTileID> tiles,
                                                             size_t maxTiles,
                                                             uint8_t minZoom,
                                                             const std::array<double, 2>& origin) {
    if (maxTiles == 0 || tiles.size() <= maxTiles) {
        return tiles;
    }

    // A parent can take its children's place when they are leaves: no tile inside it goes deeper
    // than its children, so the merge never swallows finer tiles nearer the camera and drops at
    // most three tiles. Parents of tiles at minZoom are not candidates. Each parent counts the
    // tiles directly inside it and those deeper, kept up to date as tiles merge, so a round
    // costs the depth of the tiles it touches, not the size of the cover.
    struct Count {
        int children = 0;
        int deeper = 0;
        bool queued = false;
    };
    std::unordered_map<UnwrappedTileID, Count> counts;

    // The parents ready to merge, farthest first. Ties go by tile ID, as in nearestMeshTiles.
    struct Candidate {
        double dist2;
        UnwrappedTileID id;
        bool operator<(const Candidate& other) const {
            return dist2 > other.dist2 || (dist2 == other.dist2 && id < other.id);
        }
    };
    std::set<Candidate> ready;

    const auto refresh = [&](const UnwrappedTileID& id, Count& count) {
        const bool isReady = count.children > 0 && count.deeper == 0;
        if (isReady == count.queued) {
            return;
        }
        const Candidate candidate{.dist2 = tileCentreDistance2(id, origin), .id = id};
        if (isReady) {
            ready.insert(candidate);
        } else {
            ready.erase(candidate);
        }
        count.queued = isReady;
    };

    // Adds (+1) or removes (-1) a tile in the counts of the parents it lies in; with `update`,
    // also refreshes them.
    const auto account = [&](const UnwrappedTileID& tile, int change, bool update) {
        if (tile.canonical.z <= minZoom) {
            return;
        }
        for (int z = tile.canonical.z - 1; std::cmp_greater_equal(z, minZoom); --z) {
            const UnwrappedTileID parent(tile.wrap, tile.canonical.scaledTo(static_cast<uint8_t>(z)));
            Count& count = counts[parent];
            (z == tile.canonical.z - 1 ? count.children : count.deeper) += change;
            if (update) {
                refresh(parent, count);
            }
        }
    };

    for (const auto& id : tiles) {
        account(id, 1, false);
    }
    for (auto& [id, count] : counts) {
        refresh(id, count);
    }

    // Each round takes tiles away or makes one shallower, so the loop ends by minZoom at the
    // latest.
    while (tiles.size() > maxTiles && !ready.empty()) {
        const UnwrappedTileID farthest = ready.begin()->id;
        for (const auto& child : farthest.children()) {
            if (tiles.erase(child) > 0) {
                account(child, -1, true);
            }
        }
        if (tiles.insert(farthest).second) {
            account(farthest, 1, true);
        }
    }
    return tiles;
}

void RenderTerrain::dropDrawablesOutside(const std::set<UnwrappedTileID>& cover) {
    std::unordered_set<OverscaledTileID> keep;
    for (const auto& id : cover) {
        keep.emplace(id.canonical.z, id.wrap, id.canonical);
    }
    const auto leaving = [&](gfx::Drawable& drawable) {
        return drawable.getTileID() && !keep.contains(*drawable.getTileID());
    };
    if (auto* lg = static_cast<LayerGroup*>(layerGroup.get())) {
        lg->removeDrawablesIf(leaving);
    }
    if (auto* depthLg = static_cast<LayerGroup*>(depthLayerGroup.get())) {
        if (depthLg->removeDrawablesIf(leaving) > 0) {
            depthDirty = true;
        }
    }
    for (auto it = tilesWithDrawables.begin(); it != tilesWithDrawables.end();) {
        if (!keep.contains(it->first)) {
            drawableDemCoords.erase(it->first);
            it = tilesWithDrawables.erase(it);
        } else {
            ++it;
        }
    }
}

void RenderTerrain::prepareSource(RenderOrchestrator& orchestrator) {
    // Find the DEM source if we haven't already (see header: must happen before
    // the frame's drape-target pool is built, or the first frame renders no terrain)
    if (!demSource && !impl->sourceID.empty()) {
        demSource = orchestrator.getRenderSource(impl->sourceID);
        if (!demSource) {
            Log::Warning(Event::Render, "Terrain could not find DEM source: " + impl->sourceID);
        }
    }
}

void RenderTerrain::update(RenderOrchestrator& orchestrator,
                           gfx::ShaderRegistry& shaders,
                           gfx::Context& context,
                           const TexturePool& texturePool,
                           const TransformState& state,
                           const std::shared_ptr<UpdateParameters>& updateParameters,
                           const RenderTree& /*renderTree*/,
                           UniqueChangeRequestVec& changes) {
    prepareSource(orchestrator);

    // Create layer group if we don't have one (including after rebuild)
    if (!layerGroup) {
        if (auto layerGroup_ = context.createLayerGroup(TERRAIN_LAYER_INDEX, /*initialCapacity=*/1, "terrain", false)) {
            layerGroup = std::move(layerGroup_);
            activateLayerGroup(true, changes);
        } else {
            Log::Error(Event::Render, "Failed to create terrain layer group");
            return;
        }
    }

    // Depth-pass twin of the terrain layer group; not activated in the
    // orchestrator, rendered only by renderDepth into the depth target
    if (!depthLayerGroup) {
        depthLayerGroup = context.createLayerGroup(TERRAIN_LAYER_INDEX, /*initialCapacity=*/1, "terrain-depth", false);
    }

    // Create tweaker if we don't have one
    if (!tweaker) {
        tweaker = std::make_unique<TerrainLayerTweaker>(this);
    }

    // The skirts are baked into the shared mesh, so a change of setting has to drop the mesh
    // and every tile drawable holding a buffer built from it.
    if (updateParameters && updateParameters->terrainSkirtLength != meshSkirtLength) {
        meshSkirtLength = updateParameters->terrainSkirtLength;
        mesh.reset();
        if (layerGroup) {
            layerGroup->clearDrawables();
        }
        if (depthLayerGroup) {
            depthLayerGroup->clearDrawables();
            depthDirty = true;
        }
        tilesWithDrawables.clear();
    }

    // If we don't have a DEM source, we can't create terrain drawables
    if (!demSource) {
        return;
    }

    // Get tiles from the DEM source
    auto renderTiles = demSource->getRawRenderTiles();
    if (renderTiles->empty()) {
        return;
    }

    // Cast to LayerGroup for addDrawable
    auto* lg = static_cast<LayerGroup*>(layerGroup.get());
    if (!lg) {
        return;
    }

    // Decode and cache the DEM textures of loaded DEM tiles
    ++demUpdateCounter;
    // Stitched DEMs no lookup asked for since the frame before (placement looks up before this
    // update, the layer tweakers after it) belong to tiles no longer drawn.
    std::erase_if(stitchedDEMs, [&](const auto& stitched) { return stitched.second.lastUsed + 1 < demUpdateCounter; });
    // Which DEM tiles have data, folded into a count and an order-free hash as the loop below
    // visits them, so the DEM cover's generation moves when that set changes.
    size_t demCount = 0;
    size_t demHash = 0;
    bool demChanged = false;
    for (const auto& renderTile : *renderTiles) {
        const auto& tile = renderTile.getTile();
        if (tile.kind != Tile::Kind::RasterDEM) {
            continue;
        }
        auto* demTile = const_cast<RasterDEMTile*>(static_cast<const RasterDEMTile*>(&tile));
        auto* hillshadeBucket = demTile->getBucket();
        const auto* demData = hillshadeBucket ? &hillshadeBucket->getDEMData() : nullptr;
        if (demData && demData->getImagePtr() && !demData->getImagePtr()->size.isEmpty()) {
            // All tiles come from the same raster-dem source, so they share one
            // encoding and DEM dimension
            demUnpackVector = demData->getUnpackVector();
            demDim = demData->dim;
            ++demCount;
            demHash ^= std::hash<UnwrappedTileID>{}(renderTile.id);
            if (auto existing = demTextures.find(renderTile.id); existing != demTextures.end()) {
                existing->second.lastUsed = demUpdateCounter;
                if (existing->second.revision != demData->getRevision()) {
                    // The tile's DEM changed since the upload (a reload, or a border backfilled
                    // from a neighbour that arrived later): upload it again into the same
                    // texture, so every drawable bound to it draws the new heights. Only when the
                    // DEM changes; O(1) per DEM tile per frame otherwise. The packed depth drawn
                    // from it is stale too, and a raycast that missed the old heights may hit the
                    // new ones.
                    existing->second.texture->setImage(demData->getImagePtr());
                    existing->second.revision = demData->getRevision();
                    depthDirty = true;
                    demChanged = true;
#if MLN_RENDER_BACKEND_OPENGL
                    packDEMArrayLayer(context, renderTile.id, *demData);
#endif
                }
            } else if (auto texture = createDEMTexture(context, *demData)) {
                // Keep the texture available for elevation sampling by non-draped layers
                demTextures[renderTile.id] = {.texture = texture,
                                              .dim = demData->dim,
                                              .lastUsed = demUpdateCounter,
                                              .revision = demData->getRevision()};
#if MLN_RENDER_BACKEND_OPENGL
                // Also pack this tile's DEM into the array for the (upcoming) instanced
                // depth pass. Additive: the per-tile texture above is still the fallback.
                packDEMArrayLayer(context, renderTile.id, *demData);
#endif
            }
        }
    }

    if (demChanged || demCount != demCoverCount || demHash != demCoverHash) {
        demCoverCount = demCount;
        demCoverHash = demHash;
        ++demCoverGeneration;
    }

    // The mesh (and drape) tile set: an elevation-aware, LOD-based ideal cover
    // taken straight from the view, not from the DEM source's loaded tiles - so a
    // sparse DEM's low-zoom fallback tiles don't drag the drape resolution down
    // (blurry roads) or leave uncovered areas as skirt. DEM/drape textures fall
    // back to ancestors per tile below while exact tiles load. tileCover already
    // limits to the frustum (elevation included), so no extra cull is needed.
    // Use the cover Renderer::Impl already computed for this frame when available:
    // recomputing here can yield a different set once demDim is known (see
    // setFrameMeshCover), which would desync this mesh from the drape-target pool.
    // The cover is already capped per load mode (computeMeshCover).
    std::set<UnwrappedTileID> meshTiles = frameMeshCover ? std::move(*frameMeshCover)
                                                         : computeMeshCover(state, updateParameters);
    frameMeshCover.reset();

    // Drop drawables and cached DEM textures for tiles that left the mesh tile
    // set, keeping everything else intact between frames. Renderer::Impl has already
    // dropped the drawables before building the frame's drape targets; this keeps the
    // same when the mesh cover is computed here.
    dropDrawablesOutside(meshTiles);
    std::unordered_set<OverscaledTileID> currentTiles;
    for (const auto& id : meshTiles) {
        currentTiles.emplace(id.canonical.z, id.wrap, id.canonical);
    }
    auto* depthLg = static_cast<LayerGroup*>(depthLayerGroup.get());
    // Retain cached DEM textures that are related to the current tile set so they
    // can serve as ancestor fallbacks while exact tiles load (as maplibre-gl-js
    // retains terrain tiles in its source cache); drop unrelated ones
    for (auto it = demTextures.begin(); it != demTextures.end();) {
        bool related = false;
        for (const auto& current : currentTiles) {
            const UnwrappedTileID unwrapped = current.toUnwrapped();
            if (unwrapped == it->first || unwrapped.isChildOf(it->first) || it->first.isChildOf(unwrapped)) {
                related = true;
                break;
            }
        }
        if (related) {
            it = std::next(it);
        } else {
#if MLN_RENDER_BACKEND_OPENGL
            freeDEMArrayLayer(it->first);
#endif
            it = demTextures.erase(it);
        }
    }
    // Cap the cache: ancestor/descendant relations accumulate while browsing
    // (zooming makes whole chains "related"), which previously grew past 2GB
    // of DEM textures and overflowed/OOMed. Evict least-recently-used entries
    // that were not used this frame until the cache is back under budget.
    if (demTextures.size() > maxDEMTextures) {
        std::vector<std::pair<uint64_t, UnwrappedTileID>> evictable;
        for (const auto& [id, entry] : demTextures) {
            if (entry.lastUsed != demUpdateCounter) {
                evictable.emplace_back(entry.lastUsed, id);
            }
        }
        std::sort(evictable.begin(), evictable.end());
        for (const auto& [lastUsed, id] : evictable) {
            if (demTextures.size() <= maxDEMTextures) {
                break;
            }
#if MLN_RENDER_BACKEND_OPENGL
            freeDEMArrayLayer(id);
#endif
            demTextures.erase(id);
        }
    }

    // The placeholder encodes 0 m for the DEM encoding in use; when that changes (the first DEM
    // tile of a Terrarium source arrives), drawables built on the old placeholder would sink
    // (Terrarium decodes the Mapbox zero texel to about -32 km) and are rebuilt below.
    const bool placeholderStale = placeholderDEMTexture && placeholderDEMUnpack != demUnpackVector;

    // Create terrain drawables for each mesh tile
    for (const auto& unwrapped : meshTiles) {
        const OverscaledTileID tileID(unwrapped.canonical.z, unwrapped.wrap, unwrapped.canonical);

        // Skip if the tile already has a drawable bound to its own DEM, but mark that DEM used:
        // this early-out is a tier-2 tile's only path, so the LRU could otherwise evict the
        // texture from under the drawable still sampling it.
        if (const auto existing = tilesWithDrawables.find(tileID);
            existing != tilesWithDrawables.end() && existing->second.tier == 2) {
            if (auto cached = demTextures.find(unwrapped); cached != demTextures.end()) {
                cached->second.lastUsed = demUpdateCounter;
            }
            continue;
        }

        // Resolve the DEM texture: the tile's own decoded DEM if available,
        // otherwise the closest cached ancestor as a fallback so the terrain
        // mesh stays up while the tile loads (as maplibre-gl-js does),
        // otherwise the flat placeholder
        std::shared_ptr<gfx::Texture2D> demTexture;
        // {scale, x offset, y offset, DEM dim}: maps tile-local coords (0..EXTENT)
        // into the bound DEM tile's normalized space, matching getTerrainData so
        // the terrain mesh and the elevated layers sample identically. The DEM
        // dimension rides in .w for the shader's get_elevation() call.
        std::array<float, 4> demCoords{{1.0f / util::EXTENT, 0.0f, 0.0f, static_cast<float>(demDim)}};
        uint8_t demTier = 0;
        // DEM tile whose texture / array-layer this tile uses: its zoom decides whether an
        // existing drawable is rebound, and the GL instanced-depth block reads its array layer.
        const UnwrappedTileID* demTileUsed = nullptr;

        if (auto cached = demTextures.find(unwrapped); cached != demTextures.end()) {
            cached->second.lastUsed = demUpdateCounter;
            demTexture = cached->second.texture;
            demTier = 2;
            demTileUsed = &unwrapped;
        } else {
            // Fall back to the closest cached ancestor DEM
            const UnwrappedTileID* ancestorID = nullptr;
            DEMTextureEntry* ancestorEntry = nullptr;
            int bestZoom = -1;
            for (auto& [candidate, entry] : demTextures) {
                if (candidate != unwrapped && unwrapped.isChildOf(candidate) &&
                    std::cmp_greater(candidate.canonical.z, bestZoom)) {
                    bestZoom = candidate.canonical.z;
                    ancestorID = &candidate;
                    ancestorEntry = &entry;
                    demTexture = entry.texture;
                }
            }
            if (!demTexture) {
                // No DEM at all yet: render the mesh flat with the placeholder
                // DEM so the draped map still shows (a briefly flat area is
                // less jarring than a hole in the terrain)
                demTexture = getPlaceholderDEMTexture(context);
                if (!demTexture) {
                    continue;
                }
            } else {
                ancestorEntry->lastUsed = demUpdateCounter;
                demTier = 1;
                demTileUsed = ancestorID;
                const auto mapping = demTileMapping(unwrapped.canonical, ancestorID->canonical);
                demCoords = {{mapping[0], mapping[1], mapping[2], static_cast<float>(demDim)}};
            }
        }

        // If a drawable already exists for this tile, keep it until a better DEM can be
        // bound - a higher tier, or a deeper ancestor while the tile's own DEM is missing (a
        // z13 tile first built on a z6 ancestor renders flat until z12 replaces it) - then
        // replace it
        const terrain::DEMBinding binding{
            .tier = demTier,
            .demZoom = static_cast<int8_t>(demTileUsed ? static_cast<int>(demTileUsed->canonical.z) : -1)};
        if (const auto existing = tilesWithDrawables.find(tileID); existing != tilesWithDrawables.end()) {
            if (!terrain::shouldRebind(existing->second, binding) &&
                (!placeholderStale || existing->second.tier != 0)) {
                continue;
            }
            lg->removeDrawablesIf(
                [&](gfx::Drawable& drawable) { return drawable.getTileID() && *drawable.getTileID() == tileID; });
            if (depthLg) {
                depthLg->removeDrawablesIf(
                    [&](gfx::Drawable& drawable) { return drawable.getTileID() && *drawable.getTileID() == tileID; });
                depthDirty = true;
            }
            tilesWithDrawables.erase(existing);
        }
        drawableDemCoords[tileID] = demCoords;
#if MLN_RENDER_BACKEND_OPENGL
        {
            float layer = -1.0f;
            if (demTileUsed) {
                if (const auto la = demArrayLayer.find(*demTileUsed); la != demArrayLayer.end()) {
                    layer = static_cast<float>(la->second);
                }
            }
            drawableDemLayer[tileID] = layer;
        }
#endif

        // Create terrain drawable for this tile
        const auto renderTarget = texturePool.getRenderTarget(unwrapped);
        if (!renderTarget) {
            continue;
        }
        auto drawable = createDrawableForTile(context, shaders, tileID, demTexture, renderTarget->getTexture());
        if (drawable) {
            lg->addDrawable(std::move(drawable));
            tilesWithDrawables[tileID] = binding;
#if !MLN_RENDER_BACKEND_OPENGL
            // Non-GL backends: one depth drawable per tile (no instancing path there).
            if (depthLg) {
                if (auto depthDrawable = createDrawableForTile(
                        context, shaders, tileID, demTexture, nullptr, /*depthPass=*/true)) {
                    depthLg->addDrawable(std::move(depthDrawable));
                    depthDirty = true;
                }
            }
#endif
        }
    }

    // Debug-only, off by default: gate the whole above-ground check (per-frame free-camera +
    // elevation sampling) on the flag so it costs nothing unless explicitly enabled.
    if (updateParameters && updateParameters->debugAboveGroundLog) {
        logAboveGroundMargin(state);
    }

#if MLN_RENDER_BACKEND_OPENGL
    // GL: collect the per-instance (tile, dem_coords, dem_layer) list for the whole mesh tile
    // set and rebuild the single instanced depth drawable only when that set changes (its
    // transforms refresh every frame in updateInstancedDepthUBO). Tiles without a packed DEM
    // array layer (-1) are skipped - they briefly miss depth occlusion, the same tolerance the
    // old ancestor/placeholder fallback had.
    {
        std::vector<DepthInstance> instances;
        instances.reserve(meshTiles.size());
        std::size_t sig = 0;
        for (const auto& unwrapped : meshTiles) {
            const OverscaledTileID tileID(unwrapped.canonical.z, unwrapped.wrap, unwrapped.canonical);
            const auto lc = drawableDemLayer.find(tileID);
            if (lc == drawableDemLayer.end() || lc->second < 0.0f || instances.size() >= maxDepthInstances) {
                continue;
            }
            const auto cc = drawableDemCoords.find(tileID);
            const std::array<float, 4> coords =
                cc != drawableDemCoords.end()
                    ? cc->second
                    : std::array<float, 4>{{1.0f / util::EXTENT, 0.0f, 0.0f, static_cast<float>(demDim)}};
            instances.push_back({tileID, coords, lc->second});
            util::hash_combine(sig, std::hash<OverscaledTileID>{}(tileID));
            util::hash_combine(sig, static_cast<int>(lc->second));
        }
        depthInstances = std::move(instances);
        if (sig != depthInstanceSignature) {
            depthInstanceSignature = sig;
            rebuildInstancedDepthDrawable(context, shaders);
            depthDirty = true;
        }
    }
#endif
}

void RenderTerrain::logAboveGroundMargin(const TransformState& state) {
    // Log the camera eye's clearance over the *rendered* terrain surface each frame (throttled),
    // so the flight/interaction tests can report when the sea-level-anchored camera dips below
    // terrain - the FPV underground/white artifact (TERRAIN.md Phase 4). Groundwork for a future
    // terrain-anchored camera: a concrete, measurable "how far under, and where" signal.
    if (!demSource) {
        return;
    }
    const auto fco = state.getFreeCameraOptions();
    const auto loc = fco.getLocation(); // eye Lat/Lng + altitude in metres (engine's own conversion)
    if (!fco.position || !loc) {
        return;
    }
    const double now = util::MonotonicTimer::now().count();
    if (now - lastAboveGroundLog < kAboveGroundLogInterval) {
        return;
    }
    lastAboveGroundLog = now;

    // Sample the rendered (exaggerated) terrain height directly under the eye. The eye's
    // horizontal position is the free-camera mercator x/y (0..1); getElevation walks to the best
    // loaded DEM ancestor, so a deep sample zoom just picks the finest tile available there.
    const auto& p = *fco.position;
    constexpr int sampleZoom = 14;
    const double n = std::pow(2.0, sampleZoom);
    const double fx = p[0] * n;
    const double fy = p[1] * n;
    const auto tx = static_cast<int64_t>(std::floor(fx));
    const auto ty = static_cast<int64_t>(std::floor(fy));
    const UnwrappedTileID sampleTile(static_cast<uint8_t>(sampleZoom), tx, ty);
    const auto localX = static_cast<float>((fx - static_cast<double>(tx)) * util::EXTENT);
    const auto localY = static_cast<float>((fy - static_cast<double>(ty)) * util::EXTENT);

    const double groundM = getElevationWithExaggeration(sampleTile, localX, localY);
    const double marginM = loc->altitude - groundM;
    // Only log when the camera is near or below the terrain - the interesting case, and low
    // noise (normal viewing sits km above). A DEM miss reads as groundM==0 -> large positive
    // margin, so it also stays below this gate and is not mistaken for real clearance.
    if (marginM >= kAboveGroundAlertM) {
        return;
    }
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << "ABOVE-GROUND marginM=" << marginM << " camAltM=" << loc->altitude
       << " groundM=" << groundM << " under=" << (marginM < 0.0 ? 1 : 0) << std::setprecision(4)
       << " zoom=" << state.getZoom() << " pitch=" << state.getPitch() << " lng=" << loc->location.longitude()
       << " lat=" << loc->location.latitude();
    Log::Info(Event::Render, os.str());
}

bool RenderTerrain::normalizeTileCoordinates(UnwrappedTileID& tileID, float& x, float& y) {
    constexpr float extent = static_cast<float>(util::EXTENT);
    if (x >= 0.0f && x < extent && y >= 0.0f && y < extent) {
        return true;
    }

    const auto offsetX = static_cast<int64_t>(std::floor(x / extent));
    const auto offsetY = static_cast<int64_t>(std::floor(y / extent));

    const int64_t dim = int64_t{1} << tileID.canonical.z;
    const int64_t tileY = static_cast<int64_t>(tileID.canonical.y) + offsetY;
    if (tileY < 0 || tileY >= dim) {
        // Past a pole. The tile grid does not continue, so there is nothing to sample.
        return false;
    }

    // Rounding can leave the shifted coordinate exactly on the far edge, which belongs to
    // the next tile over; keep it inside the tile we just resolved.
    x = util::clamp(x - static_cast<float>(offsetX) * extent, 0.0f, std::nextafter(extent, 0.0f));
    y = util::clamp(y - static_cast<float>(offsetY) * extent, 0.0f, std::nextafter(extent, 0.0f));

    // x wraps around the world rather than ending: hand the constructor a global tile x and
    // let it derive the wrap.
    const int64_t globalX = static_cast<int64_t>(tileID.wrap) * dim + static_cast<int64_t>(tileID.canonical.x) +
                            offsetX;
    tileID = UnwrappedTileID(tileID.canonical.z, globalX, tileY);
    return true;
}

float RenderTerrain::getElevation(const UnwrappedTileID& tileID, float x, float y) const {
    return findElevation(tileID, x, y).value_or(0.0f);
}

const RenderTile* RenderTerrain::findDEMTile(const UnwrappedTileID& tileID) const {
    if (!demSource) {
        return nullptr;
    }
    // The DEM tile matching the requested tile, or its closest available ancestor. Tiles with no
    // DEM data are skipped, so a covering ancestor that has some answers instead.
    const auto renderTiles = demSource->getRawRenderTiles();
    const RenderTile* demRenderTile = nullptr;
    int bestZoom = -1;
    for (const auto& renderTile : *renderTiles) {
        const UnwrappedTileID& candidate = renderTile.id;
        if ((candidate == tileID || tileID.isChildOf(candidate)) && std::cmp_greater(candidate.canonical.z, bestZoom) &&
            demDataOf(renderTile)) {
            bestZoom = candidate.canonical.z;
            demRenderTile = &renderTile;
        }
    }
    return demRenderTile;
}

RenderTerrain::TileElevation::TileElevation(
    std::shared_ptr<const DEMData> dem_, float scale_, float dx_, float dy_, float exaggeration_)
    : dem(std::move(dem_)),
      scale(scale_),
      dx(dx_),
      dy(dy_),
      exaggeration(exaggeration_) {}

float RenderTerrain::TileElevation::height(float x, float y) const {
    return sampleDEM(*dem, {.scale = scale, .dx = dx, .dy = dy}, x, y);
}

std::optional<RenderTerrain::TileElevation> RenderTerrain::getTileElevation(const UnwrappedTileID& tileID) const {
    const float exaggeration = getExaggeration();
    if (exaggeration <= 0.0f) {
        return std::nullopt;
    }
    // A coordinate outside the tile, which only Tile mode's buffer-zone symbols have, needs no
    // neighbour lookup: the shaders sample this tile's DEM there too, into its border and then
    // clamped, and so does sampleDEM.
    if (const RenderTile* demRenderTile = findDEMTile(tileID)) {
        const auto off = demSubTileOffset(tileID.canonical, demRenderTile->id.canonical);
        return TileElevation(sharedDEMDataOf(*demRenderTile), off.scale, off.dx, off.dy, exaggeration);
    }
    if (const StitchedDEM* stitched = findStitchedDEM(tileID)) {
        return TileElevation(stitched->dem, 1.0f, 0.0f, 0.0f, exaggeration);
    }
    return std::nullopt;
}

RenderTerrain::StitchedDEM* RenderTerrain::findStitchedDEM(const UnwrappedTileID& tileID) const {
    if (!demSource) {
        return nullptr;
    }
    const auto renderTiles = demSource->getRawRenderTiles();
    const auto under = [&](const RenderTile& renderTile) {
        return renderTile.id.canonical.z > tileID.canonical.z && renderTile.id.isChildOf(tileID);
    };
    // Still current while the same DEM tiles under it hold the same data, in the same order.
    const auto existing = stitchedDEMs.find(tileID);
    bool current = existing != stitchedDEMs.end();
    size_t count = 0;
    for (const auto& renderTile : *renderTiles) {
        const DEMData* data = under(renderTile) ? demDataOf(renderTile) : nullptr;
        if (data) {
            current = current && count < existing->second.sources.size() &&
                      existing->second.sources[count].get() == data &&
                      existing->second.revisions[count] == data->getRevision();
            ++count;
        }
    }
    if (current && count == existing->second.sources.size()) {
        existing->second.lastUsed = demUpdateCounter;
        return &existing->second;
    }
    if (count == 0) {
        if (existing != stitchedDEMs.end()) {
            stitchedDEMs.erase(existing);
        }
        return nullptr;
    }

    StitchedDEM stitched;
    stitched.sources.reserve(count);
    stitched.revisions.reserve(count);
    std::vector<StitchSource> coarseToFine;
    coarseToFine.reserve(count);
    for (const auto& renderTile : *renderTiles) {
        if (const DEMData* data = under(renderTile) ? demDataOf(renderTile) : nullptr) {
            stitched.sources.push_back(sharedDEMDataOf(renderTile));
            stitched.revisions.push_back(data->getRevision());
            coarseToFine.push_back({.id = renderTile.id.canonical, .dem = data});
        }
    }
    std::stable_sort(coarseToFine.begin(), coarseToFine.end(), [](const StitchSource& a, const StitchSource& b) {
        return a.id.z < b.id.z;
    });
    stitched.dem = stitchDEM(tileID.canonical, coarseToFine);
    stitched.lastUsed = demUpdateCounter;
    StitchedDEM& slot = stitchedDEMs[tileID];
    slot = std::move(stitched);
    return &slot;
}

const RenderTile* RenderTerrain::findDEMTile(const UnwrappedTileID& tileID, DEMTileMemo& memo) const {
    if (memo.tileID != tileID) {
        memo.tileID = tileID;
        memo.demTile = findDEMTile(tileID);
    }
    return memo.demTile;
}

std::optional<float> RenderTerrain::findElevation(const UnwrappedTileID& tileID, float x, float y) const {
    DEMTileMemo memo;
    return findElevation(tileID, x, y, memo);
}

std::optional<float> RenderTerrain::findElevation(UnwrappedTileID tileID, float x, float y, DEMTileMemo& memo) const {
    if (!normalizeTileCoordinates(tileID, x, y)) {
        return std::nullopt;
    }
    const RenderTile* demRenderTile = findDEMTile(tileID, memo);
    if (!demRenderTile) {
        return std::nullopt;
    }
    // Sampled within this call, before anything can replace the tile's DEM: no ref is needed.
    return sampleDEM(*demDataOf(*demRenderTile), demSubTileOffset(tileID.canonical, demRenderTile->id.canonical), x, y);
}

float RenderTerrain::getElevationWithExaggeration(const UnwrappedTileID& tileID, float x, float y) const {
    return getElevation(tileID, x, y) * getExaggeration();
}

double RenderTerrain::getElevationForLatLng(const LatLng& latLng) const {
    if (!demSource) {
        return 0.0;
    }

    // Sample as deep as the finest DEM tile loaded: getElevation walks up to the closest
    // covering ancestor, but never down, so sampling shallower than the DEM reads nothing.
    const int sampleZoom = finestDEMZoom();
    if (sampleZoom < 0) {
        return 0.0;
    }

    const double n = std::pow(2.0, sampleZoom);
    // The int-zoom overload of project() returns tile units directly (0..2^zoom); the
    // same-named double-scale overload returns pixels. Do not divide by the tile size.
    const auto point = Projection::project(latLng, sampleZoom);
    const double fx = point.x;
    const double fy = point.y;
    const auto tx = static_cast<int64_t>(std::floor(fx));
    const auto ty = static_cast<int64_t>(std::floor(fy));
    if (ty < 0 || static_cast<double>(ty) >= n) {
        return 0.0; // past a pole
    }

    const UnwrappedTileID sampleTile(static_cast<uint8_t>(sampleZoom), tx, ty);
    const auto localX = static_cast<float>((fx - static_cast<double>(tx)) * util::EXTENT);
    const auto localY = static_cast<float>((fy - static_cast<double>(ty)) * util::EXTENT);
    return getElevationWithExaggeration(sampleTile, localX, localY);
}

int RenderTerrain::finestDEMZoom() const {
    if (!demSource) {
        return -1;
    }
    int zoom = -1;
    for (const auto& renderTile : *demSource->getRawRenderTiles()) {
        if (demDataOf(renderTile)) {
            zoom = std::max(zoom, static_cast<int>(renderTile.id.canonical.z));
        }
    }
    return zoom;
}

std::optional<double> RenderTerrain::findElevationForLatLng(const LatLng& latLng) const {
    return findElevationForLatLng(latLng, finestDEMZoom());
}

std::optional<double> RenderTerrain::findElevationForLatLng(const LatLng& latLng, int sampleZoom) const {
    DEMTileMemo memo;
    return findElevationForLatLng(latLng, sampleZoom, memo);
}

std::optional<double> RenderTerrain::findElevationForLatLng(const LatLng& latLng,
                                                            int sampleZoom,
                                                            DEMTileMemo& memo) const {
    if (sampleZoom < 0) {
        return std::nullopt;
    }
    const double n = std::pow(2.0, sampleZoom);
    const auto point = Projection::project(latLng, sampleZoom);
    const auto tx = static_cast<int64_t>(std::floor(point.x));
    const auto ty = static_cast<int64_t>(std::floor(point.y));
    if (ty < 0 || static_cast<double>(ty) >= n) {
        return std::nullopt;
    }
    const UnwrappedTileID sampleTile(static_cast<uint8_t>(sampleZoom), tx, ty);
    const auto localX = static_cast<float>((point.x - static_cast<double>(tx)) * util::EXTENT);
    const auto localY = static_cast<float>((point.y - static_cast<double>(ty)) * util::EXTENT);
    const auto elevation = findElevation(sampleTile, localX, localY, memo);
    if (!elevation) {
        return std::nullopt;
    }
    return *elevation * getExaggeration();
}

std::optional<RenderTerrain::RaycastSetup> RenderTerrain::prepareRaycast(const TransformState& state) const {
    if (!demSource) {
        return std::nullopt;
    }
    const Size size = state.getSize();
    if (size.isEmpty()) {
        return std::nullopt;
    }

    // The elevation range of the loaded DEM (exaggerated), which the surface cannot leave. It
    // always spans sea level, as getElevationForLatLng reads 0 where no DEM covers the point.
    const double exaggeration = getExaggeration();
    double minElevation = 0.0;
    double maxElevation = 0.0;
    bool anyDEM = false;
    for (const auto& renderTile : *demSource->getRawRenderTiles()) {
        const auto& tile = renderTile.getTile();
        if (tile.kind != Tile::Kind::RasterDEM) {
            continue;
        }
        const auto* bucket = static_cast<const RasterDEMTile&>(tile).getBucket();
        if (!bucket || !bucket->getDEMData().getImagePtr()) {
            continue;
        }
        anyDEM = true;
        minElevation = std::min(minElevation, bucket->getDEMData().getMinElevation() * exaggeration);
        maxElevation = std::max(maxElevation, bucket->getDEMData().getMaxElevation() * exaggeration);
    }
    // No DEM tile to sample: no ray can come down below the surface.
    const int sampleZoom = finestDEMZoom();
    if (!anyDEM || sampleZoom < 0) {
        return std::nullopt;
    }

    mat4 inverse;
    if (matrix::invert(inverse, state.getProjectionMatrix())) {
        return std::nullopt;
    }
    return RaycastSetup{.inverseProjection = inverse,
                        .size = size,
                        .scale = state.getScale(),
                        .minElevation = minElevation,
                        .maxElevation = maxElevation,
                        .sampleZoom = sampleZoom};
}

std::optional<LatLng> RenderTerrain::raycast(const RaycastSetup& setup, const ScreenCoordinate& point) const {
    const auto hit = raycastPoint(setup, point);
    if (!hit) {
        return std::nullopt;
    }
    return Projection::unproject({(*hit)[0], (*hit)[1]}, setup.scale, LatLng::Unwrapped);
}

std::optional<vec3> RenderTerrain::raycastPoint(const RaycastSetup& setup, const ScreenCoordinate& point) const {
    // The ray from the near (clip z -1) to the far (clip z 1) plane, in world pixels with z in
    // metres - the space TransformState's projection matrix maps from.
    const double ndcX = point.x / setup.size.width * 2.0 - 1.0;
    const double ndcY = point.y / setup.size.height * 2.0 - 1.0;
    const auto unproject = [&](double ndcZ) {
        vec4 out;
        matrix::transformMat4(out, vec4{{ndcX, ndcY, ndcZ, 1.0}}, setup.inverseProjection);
        return vec3{{out[0] / out[3], out[1] / out[3], out[2] / out[3]}};
    };
    const vec3 near = unproject(-1.0);
    const vec3 far = unproject(1.0);
    const auto t = raycastSegment(setup, near, far);
    if (!t) {
        return std::nullopt;
    }
    return vec3{
        {near[0] + *t * (far[0] - near[0]), near[1] + *t * (far[1] - near[1]), near[2] + *t * (far[2] - near[2])}};
}

std::optional<double> RenderTerrain::raycastAltitude(const TransformState& state,
                                                     const std::array<vec3, 2>& ray) const {
    const auto setup = prepareRaycast(state);
    if (!setup) {
        return std::nullopt;
    }
    // From Mercator units to the world pixels raycastSegment marches in.
    const double worldSize = Projection::worldSize(setup->scale);
    const vec3 near{{ray[0][0] * worldSize, ray[0][1] * worldSize, ray[0][2]}};
    const vec3 far{{ray[1][0] * worldSize, ray[1][1] * worldSize, ray[1][2]}};
    const auto t = raycastSegment(*setup, near, far);
    if (!t) {
        return std::nullopt;
    }
    return near[2] + *t * (far[2] - near[2]);
}

std::optional<double> RenderTerrain::raycastSegment(const RaycastSetup& setup,
                                                    const vec3& near,
                                                    const vec3& far) const {
    const double minElevation = setup.minElevation;
    const double maxElevation = setup.maxElevation;
    const double dx = far[0] - near[0];
    const double dy = far[1] - near[1];
    const double dz = far[2] - near[2];

    const auto at = [&](double t) {
        return Projection::unproject({near[0] + t * dx, near[1] + t * dy}, setup.scale, LatLng::Unwrapped);
    };
    // As gl-js `isBelowTerrainSample`: ground no loaded DEM covers is never hit. The ray's samples
    // walk through a few DEM tiles in order, so each tile is looked up once, not once a sample.
    constexpr double hitEpsilonMeters = 1e-6;
    DEMTileMemo memo;
    const auto belowTerrain = [&](double t) {
        const auto elevation = findElevationForLatLng(at(t), setup.sampleZoom, memo);
        return elevation && near[2] + t * dz <= *elevation + hitEpsilonMeters;
    };

    // Clip the segment to the slab between the lowest and highest terrain.
    double tStart = 0.0;
    double tEnd = 1.0;
    if (dz == 0.0) {
        if (near[2] > maxElevation || near[2] < minElevation) {
            return std::nullopt;
        }
    } else {
        const double tHigh = (maxElevation - near[2]) / dz;
        const double tLow = (minElevation - near[2]) / dz;
        tStart = std::max(tStart, std::min(tHigh, tLow));
        tEnd = std::min(tEnd, std::max(tHigh, tLow));
        if (tStart > tEnd) {
            return std::nullopt;
        }
    }

    constexpr double targetStepWorldPixels = 4.0;
    constexpr int maxSamples = 512;
    constexpr double bisectEpsilonWorldPixels = 1e-3;
    const double horizontalLength = std::hypot(dx, dy);
    const int samples = std::clamp(
        static_cast<int>(std::ceil(horizontalLength * (tEnd - tStart) / targetStepWorldPixels)), 1, maxSamples);

    // Only a crossing from above the surface counts: a ray that starts inside the terrain
    // (camera below the surface) hits where it next comes out on top and back down. The start
    // state is read at the near plane, not at the clipped start, which can sit exactly on the
    // highest terrain.
    constexpr int maxBisections = 40;
    bool aboveTerrain = !belowTerrain(0.0);
    double previousT = 0.0;
    for (int i = 0; i <= samples; ++i) {
        const double t = tStart + (tEnd - tStart) * i / samples;
        if (!aboveTerrain) {
            aboveTerrain = !belowTerrain(t);
        } else if (belowTerrain(t)) {
            double lo = previousT;
            double hi = t;
            const double tolerance = horizontalLength > 0.0 ? bisectEpsilonWorldPixels / horizontalLength : 0.0;
            for (int j = 0; j < maxBisections && hi - lo > tolerance; ++j) {
                const double mid = (lo + hi) / 2.0;
                (belowTerrain(mid) ? hi : lo) = mid;
            }
            return hi;
        }
        previousT = t;
    }
    return std::nullopt;
}

std::optional<RenderTerrain::TerrainData> RenderTerrain::getTerrainData(const UnwrappedTileID& tileID,
                                                                        gfx::Context& context) {
    // The DEM getTileElevation samples (placement lifts collision boxes by it), so a symbol is
    // drawn where it collides and is queried. First the DEM tile matching the requested tile, or
    // its closest loaded ancestor with data, as findDEMTile resolves it.
    if (const RenderTile* demRenderTile = findDEMTile(tileID)) {
        auto entry = demTextures.find(demRenderTile->id);
        if (entry == demTextures.end()) {
            // update() uploads every loaded DEM tile, but may since have evicted this one
            auto texture = createDEMTexture(context, *demDataOf(*demRenderTile));
            if (!texture) {
                return std::nullopt;
            }
            context.renderingStats().numDEMTexturesRemade++;
            entry = demTextures
                        .emplace(demRenderTile->id,
                                 DEMTextureEntry{.texture = std::move(texture),
                                                 .dim = demDataOf(*demRenderTile)->dim,
                                                 .lastUsed = demUpdateCounter,
                                                 .revision = demDataOf(*demRenderTile)->getRevision()})
                        .first;
#if MLN_RENDER_BACKEND_OPENGL
            // Pack it into the depth pass's array too, as update() does for a new texture: the
            // entry is stored at the DEM's current revision, so update() will not pack it later,
            // and a mesh tile drawn on it would be left out of the terrain depth.
            packDEMArrayLayer(context, demRenderTile->id, *demDataOf(*demRenderTile));
#endif
        }
        entry->second.lastUsed = demUpdateCounter;
        // Map tile-local coordinates (0..EXTENT) of the requested tile into
        // normalized coordinates (0..1) of the (possibly ancestor) DEM tile
        const auto mapping = demTileMapping(tileID.canonical, demRenderTile->id.canonical);
        return TerrainData{
            .demTexture = entry->second.texture,
            .demCoords = {{mapping[0], mapping[1], mapping[2], 0.0f}},
            .demDim = static_cast<float>(entry->second.dim),
        };
    }

    // A DEM finer than the tile (a 256 px DEM is a zoom deeper than 512 px vector tiles): bind
    // the DEM stitched from the tile's loaded descendants, which covers exactly this tile.
    StitchedDEM* stitched = findStitchedDEM(tileID);
    if (!stitched) {
        return std::nullopt;
    }
    if (!stitched->texture) {
        stitched->texture = createDEMTexture(context, *stitched->dem);
        if (!stitched->texture) {
            return std::nullopt;
        }
    }
    return TerrainData{
        .demTexture = stitched->texture,
        .demCoords = {{1.0f / util::EXTENT, 0.0f, 0.0f, 0.0f}},
        .demDim = static_cast<float>(stitched->dem->dim),
    };
}

const std::shared_ptr<gfx::Texture2D>& RenderTerrain::getPlaceholderDEMTexture(gfx::Context& context) {
    if (!placeholderDEMTexture || placeholderDEMUnpack != demUnpackVector) {
        // One texel at 0 m in the current encoding: an all-zero one would sink a tile without
        // DEM out of view, leaving its drape undrawn.
        const auto zero = elevationTexel(demUnpackVector, 0.0);
        auto image = std::make_shared<PremultipliedImage>(Size{1, 1}, zero.data(), zero.size());
        placeholderDEMUnpack = demUnpackVector;
        placeholderDEMTexture = context.createTexture2D();
        placeholderDEMTexture->setImage(image);
        placeholderDEMTexture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Nearest,
                                                        .wrapU = gfx::TextureWrapType::Clamp,
                                                        .wrapV = gfx::TextureWrapType::Clamp});
    }
    return placeholderDEMTexture;
}

void RenderTerrain::renderDepth(RenderOrchestrator& orchestrator,
                                const RenderTree& renderTree,
                                PaintParameters& parameters) {
    if (!depthLayerGroup || depthLayerGroup->empty()) {
        return;
    }
    prepareDepthTarget(parameters);
    if (!depthRenderTarget) {
        return;
    }
    // The packed-depth output is a function of the camera projection, the terrain mesh set and
    // the DEM heights (update() marks a DEM change dirty) only. When none changed since the last
    // depth render, the existing depth texture is still correct - skip the whole pass. This is
    // what makes a static scene cheap; the depth is redrawn only on camera movement or a
    // mesh/tile/DEM change. (From 604f293; without this gate the pass ran every frame, which is
    // the state that flickered.)
    const mat4& proj = parameters.transformParams.projMatrix;
    const bool cameraMoved = !lastDepthProjMatrix || *lastDepthProjMatrix != proj;
    if (!depthDirty && !cameraMoved) {
        return;
    }

#if MLN_RENDER_BACKEND_OPENGL
    // Instanced depth pass: refresh the per-instance UBO array (camera-dependent transforms)
    // and bind the packed DEM array to the unit the shader's u_dem_array sampler expects. The
    // instanced drawable carries no gfx textures, so nothing else touches this unit during the
    // depth render. (Bind integration is the main on-device shakeout item - Texture2DArray is
    // GL-only and outside the gfx texture abstraction.)
    updateInstancedDepthUBO(parameters);
    // The DEM array is bound by the instanced drawable itself (DrawableGL::setArrayTexture ->
    // bindTextures), so no manual bind here.
#endif
    depthRenderTarget->render(orchestrator, renderTree, parameters);
    parameters.context.renderingStats().numDepthPasses++;
    lastDepthProjMatrix = proj;
    depthDirty = false;
}

void RenderTerrain::prepareDepthTarget(PaintParameters& parameters) {
    // Called at the start of the render (before the upload phase) as well as from
    // renderDepth, so the depth target already exists when the symbol tweaker binds
    // getDepthTexture() for this frame. Creating it lazily in renderDepth alone left
    // the symbols bound to the far-plane placeholder for that frame - permanently so
    // in single-frame renders like the render tests, where terrain occlusion then
    // never engaged.
    const Size size = parameters.backend.getDefaultRenderable().getSize();
    if (size.isEmpty()) {
        // Early frames can run before the surface has a real size; a degenerate
        // render target here would hand the symbol tweaker a broken texture
        return;
    }
    bool fresh = false;
    if (!depthRenderTarget || !depthRenderTarget->getTexture() || depthRenderTarget->getTexture()->getSize() != size) {
        depthRenderTarget = parameters.context.createRenderTarget(
            size, gfx::TextureChannelDataType::UnsignedByte, /*stencil=*/false);
        depthLayerGroupAttached = nullptr;
        if (!depthRenderTarget) {
            return;
        }
        fresh = true;
        // Far plane everywhere the terrain does not cover (unpack_depth(1,1,1,1) ~ 1.0)
        depthRenderTarget->setClearColor(Color::white());
        // The packed window depth must be the NDC depth itself on every backend, as
        // unpack_depth assumes: no layer depth range compressing it (#4868 applies the
        // range on Metal, Vulkan and WebGPU too).
        depthRenderTarget->setFullDepthRange(true);
        // The packed digits must not be filtered: blending neighbouring texels'
        // channels decodes to garbage (offscreen textures default to Linear)
        depthRenderTarget->getTexture()->setSamplerConfiguration({.filter = gfx::TextureFilterType::Nearest,
                                                                  .wrapU = gfx::TextureWrapType::Clamp,
                                                                  .wrapV = gfx::TextureWrapType::Clamp});
    }
    // Attach the depth layer group once per target; it may not have existed yet when the
    // target was first created on an early frame. Attaching on every call marked the pass
    // dirty on every frame, so the skip in renderDepth never fired.
    if (depthLayerGroup && (fresh || depthLayerGroupAttached != depthLayerGroup.get())) {
        depthRenderTarget->addLayerGroup(depthLayerGroup, /*replace=*/true);
        depthLayerGroupAttached = depthLayerGroup.get();
        depthDirty = true; // fresh target must be drawn
    }
}

const std::shared_ptr<gfx::Texture2D>& RenderTerrain::getDepthTexture(gfx::Context& context) {
    // The target holds depth only while the depth pass has drawables: renderDepth then draws it
    // later this frame, or drew it for the same drawables and camera before. With none (no DEM
    // tile has arrived yet, or every one left) the pass never runs, and the target's texture was
    // never drawn into: on Vulkan and Metal it has no image yet, which Vulkan dereferenced when
    // a symbol bound it. Nothing occludes then, so the symbols test against the far plane.
    if (depthRenderTarget && depthRenderTarget->getTexture() && depthLayerGroup && !depthLayerGroup->empty()) {
        return depthRenderTarget->getTexture();
    }
    if (!placeholderDepthTexture) {
        // Far-plane packed depth: symbols compare as visible until the pass runs
        auto image = std::make_shared<PremultipliedImage>(Size{1, 1});
        std::memset(image->data.get(), 0xFF, image->bytes());
        placeholderDepthTexture = context.createTexture2D();
        placeholderDepthTexture->setImage(image);
        placeholderDepthTexture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Nearest,
                                                          .wrapU = gfx::TextureWrapType::Clamp,
                                                          .wrapV = gfx::TextureWrapType::Clamp});
    }
    return placeholderDepthTexture;
}

float RenderTerrain::getExaggeration() const {
    return impl->exaggeration;
}

const std::string& RenderTerrain::getSourceID() const {
    return impl->sourceID;
}

bool RenderTerrain::isEnabled() const {
    return !impl->sourceID.empty();
}

const RenderTerrain::TerrainMesh& RenderTerrain::getMesh(gfx::Context& context) {
    if (!mesh) {
        generateMesh(context);
    }
    return *mesh;
}

const RenderTerrain::TerrainMesh& RenderTerrain::getDepthMesh(gfx::Context& context) {
    // The instanced depth pass reuses the full terrain mesh; the source PR's coarser
    // depth-only mesh (getDepthMesh/buildMesh) is a separable optimization not pulled here.
    return getMesh(context);
}

void RenderTerrain::generateMesh(gfx::Context& /*context*/) {
    // A regular grid mesh (reused for every tile, displaced by the DEM in the
    // vertex shader) plus a skirt: each tile edge is duplicated into a curtain
    // that the shader drops by u_ele_delta, hiding the cracks between neighbouring
    // tiles at different zoom levels. Ported from maplibre-gl-js Terrain
    // getTerrainMesh()/_buildSkirts(). TerrainSkirtLength::None builds the bare grid.
    const size_t gridSize = MESH_SIZE;
    const size_t vps = gridSize + 1; // vertices per side
    const float step = static_cast<float>(util::EXTENT) / static_cast<float>(gridSize);

    std::vector<int16_t> vertices;
    std::vector<uint16_t> indices;

    // Each vertex is 4 shorts: x, y, skirt flag (0 = surface, 1 = skirt),
    // unused. uv is derived from x,y in the shader, so the 3rd/4th shorts are free
    // to carry the skirt flag (the native analog of gl-js Pos3d.z).
    const auto addVert = [&](float x, float y, int16_t skirt) {
        vertices.push_back(static_cast<int16_t>(x));
        vertices.push_back(static_cast<int16_t>(y));
        vertices.push_back(skirt);
        vertices.push_back(0);
    };

    // Surface grid
    for (size_t y = 0; y < vps; ++y) {
        for (size_t x = 0; x < vps; ++x) {
            addVert(x * step, y * step, 0);
        }
    }
    for (size_t y = 0; y < gridSize; ++y) {
        for (size_t x = 0; x < gridSize; ++x) {
            const uint16_t topLeft = static_cast<uint16_t>(y * vps + x);
            const uint16_t topRight = static_cast<uint16_t>(topLeft + 1);
            const uint16_t bottomLeft = static_cast<uint16_t>((y + 1) * vps + x);
            const uint16_t bottomRight = static_cast<uint16_t>(bottomLeft + 1);
            indices.push_back(topLeft);
            indices.push_back(bottomLeft);
            indices.push_back(topRight);
            indices.push_back(topRight);
            indices.push_back(bottomLeft);
            indices.push_back(bottomRight);
        }
    }

    // Skirts are baked into the shared mesh; TerrainSkirtLength::None leaves the bare grid.
    if (meshSkirtLength != TerrainSkirtLength::None) {
        // Top/bottom skirt rows (reference the grid's top/bottom edge rows)
        const auto extent = static_cast<float>(util::EXTENT);
        const uint16_t offsetTop = static_cast<uint16_t>(vertices.size() / 4);
        const uint16_t offsetTopEdge = 0;
        const uint16_t offsetBottom = static_cast<uint16_t>(offsetTop + vps);
        const uint16_t offsetBottomEdge = static_cast<uint16_t>(vps * gridSize);
        for (size_t x = 0; x < vps; ++x) {
            addVert(x * step, 0.0f, 1);
        }
        for (size_t x = 0; x < vps; ++x) {
            addVert(x * step, extent, 1);
        }
        for (size_t x = 0; x < gridSize; ++x) {
            indices.insert(indices.end(),
                           {static_cast<uint16_t>(offsetBottomEdge + x),
                            static_cast<uint16_t>(offsetBottom + x),
                            static_cast<uint16_t>(offsetBottom + x + 1),
                            static_cast<uint16_t>(offsetBottomEdge + x),
                            static_cast<uint16_t>(offsetBottom + x + 1),
                            static_cast<uint16_t>(offsetBottomEdge + x + 1),
                            static_cast<uint16_t>(offsetTopEdge + x),
                            static_cast<uint16_t>(offsetTop + x + 1),
                            static_cast<uint16_t>(offsetTop + x),
                            static_cast<uint16_t>(offsetTopEdge + x),
                            static_cast<uint16_t>(offsetTopEdge + x + 1),
                            static_cast<uint16_t>(offsetTop + x + 1)});
        }

        // Left/right skirt frames (self-contained strips of paired surface/skirt verts)
        const uint16_t offsetLeft = static_cast<uint16_t>(vertices.size() / 4);
        const uint16_t offsetRight = static_cast<uint16_t>(offsetLeft + vps * 2);
        for (int edge = 0; edge <= 1; ++edge) {
            for (size_t y = 0; y < vps; ++y) {
                for (int16_t z = 0; z <= 1; ++z) {
                    addVert(static_cast<float>(edge) * extent, y * step, z);
                }
            }
        }
        for (size_t y = 0; y < gridSize * 2; y += 2) {
            indices.insert(indices.end(),
                           {static_cast<uint16_t>(offsetLeft + y),
                            static_cast<uint16_t>(offsetLeft + y + 1),
                            static_cast<uint16_t>(offsetLeft + y + 3),
                            static_cast<uint16_t>(offsetLeft + y),
                            static_cast<uint16_t>(offsetLeft + y + 3),
                            static_cast<uint16_t>(offsetLeft + y + 2),
                            static_cast<uint16_t>(offsetRight + y),
                            static_cast<uint16_t>(offsetRight + y + 3),
                            static_cast<uint16_t>(offsetRight + y + 1),
                            static_cast<uint16_t>(offsetRight + y),
                            static_cast<uint16_t>(offsetRight + y + 2),
                            static_cast<uint16_t>(offsetRight + y + 3)});
        }
    }

    mesh = TerrainMesh{.vertexBuffer = nullptr, // created when building the drawable
                       .indexBuffer = nullptr,  // created when building the drawable
                       .vertexCount = vertices.size() / 4,
                       .indexCount = indices.size(),
                       .vertices = std::move(vertices),
                       .indices = std::move(indices)};
}

std::shared_ptr<gfx::Texture2D> RenderTerrain::createDEMTexture(gfx::Context& context, const DEMData& demData) {
    // Get the DEM image data
    const auto& imagePtr = demData.getImagePtr();
    if (!imagePtr || imagePtr->size.isEmpty()) {
        Log::Warning(Event::Render, "DEM data has no image");
        return nullptr;
    }

    // Create a new texture
    auto texture = context.createTexture2D();
    if (!texture) {
        Log::Error(Event::Render, "Failed to create DEM texture");
        return nullptr;
    }

    // Set the image data
    texture->setImage(imagePtr);

    // Nearest filtering: the packed Terrain-RGB/Terrarium DEM cannot be hardware
    // interpolated (blending the encoded bytes does not blend the decoded
    // elevations), so shaders decode each texel and interpolate in meters via
    // get_elevation(). This matches maplibre-gl-js, which binds the DEM NEAREST.
    texture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Nearest,
                                      .wrapU = gfx::TextureWrapType::Clamp,
                                      .wrapV = gfx::TextureWrapType::Clamp});

    return texture;
}

std::unique_ptr<gfx::Drawable> RenderTerrain::createDrawableForTile(gfx::Context& context,
                                                                    gfx::ShaderRegistry& shaders,
                                                                    const OverscaledTileID& tileID,
                                                                    std::shared_ptr<gfx::Texture2D> demTexture,
                                                                    std::shared_ptr<gfx::Texture2D> mapTexture,
                                                                    bool depthPass) {
    // Ensure mesh is generated
    const auto& terrainMesh = getMesh(context);

    if (terrainMesh.vertices.empty() || terrainMesh.indices.empty()) {
        Log::Error(Event::Render, "Terrain mesh is empty, cannot create drawable");
        return nullptr;
    }

    // Get terrain shader
    auto terrainShader = context.getGenericShader(shaders, depthPass ? "TerrainDepthShader" : "TerrainShader");
    if (!terrainShader) {
        // The depth shader is not registered on all backends yet; symbols
        // then sample the far-plane placeholder and stay visible
        if (!depthPass) {
            Log::Error(Event::Render, "Terrain shader not found");
        }
        return nullptr;
    }

    // Create drawable builder
    auto builder = context.createDrawableBuilder(depthPass ? "terrain-depth-tile" : "terrain-tile");
    if (!builder) {
        Log::Error(Event::Render, "Failed to create drawable builder for terrain tile");
        return nullptr;
    }

    // The drape pass uses the Translucent render pass because it renders in
    // forward order (high index = front), unlike Opaque which renders reversed.
    builder->setShader(terrainShader);
    builder->setRenderPass(RenderPass::Translucent);
    if (depthPass) {
        // The depth pass renders packed depth with real depth testing so the
        // nearest surface wins, into the terrain depth target (renderDepth)
        builder->setDepthType(gfx::DepthMaskType::ReadWrite);
        builder->setColorMode(gfx::ColorMode::unblended());
        builder->setEnableDepth(true);
        builder->setIs3D(true);
    } else {
        // The terrain surface is 3D geometry, so it tests and writes depth: nearer
        // terrain occludes farther terrain, and - crucially - occludes the skirt
        // curtains hanging below each tile edge, so the skirts only show through
        // the cracks they exist to fill rather than drawing over the surface.
        //
        // Symbols (and other layers that occlude against terrain via the depth
        // texture) must not main-depth-test against this surface, or they would be
        // culled by the terrain they sit on - the symbol tweaker disables their
        // depth test while terrain is enabled.
        // Match maplibre-gl-js / Mapbox: the terrain surface is opaque 3D geometry
        // drawn with a depth test+write (LEQUAL, ReadWrite), not the earlier
        // depth-off / "2D for now" hack. On tiled GPUs (this device is PowerVR) opaque
        // depth-tested geometry is eligible for hidden-surface removal, so occluded
        // fragments skip the drape sample instead of always running it.
        builder->setDepthType(gfx::DepthMaskType::ReadWrite);
        builder->setColorMode(gfx::ColorMode::unblended());
        builder->setEnableDepth(true);
        builder->setIs3D(true);
    }

    // Set vertex data - copy vertices to raw buffer
    std::vector<uint8_t> vertexData(terrainMesh.vertices.size() * sizeof(int16_t));
    std::memcpy(vertexData.data(), terrainMesh.vertices.data(), vertexData.size());
    builder->setRawVertices(std::move(vertexData), terrainMesh.vertexCount, gfx::AttributeDataType::Short4);

    // Set index data and segments
    // Create a single segment covering the entire terrain mesh
    SegmentVector segments;
    segments.emplace_back(0,                       // vertex offset
                          0,                       // index offset
                          terrainMesh.vertexCount, // vertex count
                          terrainMesh.indexCount); // index count

    std::vector<uint16_t> indexData = terrainMesh.indices;
    builder->setSegments(gfx::Triangles(), std::move(indexData), segments.data(), segments.size());

    // Set the DEM texture
    if (demTexture) {
        builder->setTexture(demTexture, 0); // Texture index 0 for DEM
    }

    // The depth pass samples only the DEM and writes packed depth, so it has no
    // map texture by design; only the draped pass binds the drape render target
    if (!depthPass) {
        if (mapTexture) {
            builder->setTexture(mapTexture, 1); // Texture index 1 for map
        } else {
            Log::Warning(Event::Render, "No drape texture for terrain tile " + util::toString(tileID));
        }
    }

    // Flush to create the drawable
    builder->flush(context);

    // Get the drawable
    auto drawables = builder->clearDrawables();
    if (drawables.empty()) {
        Log::Error(Event::Render, "Failed to create terrain drawable for tile");
        return nullptr;
    }

    // Set tile ID on the drawable
    auto& drawable = drawables[0];
    drawable->setTileID(tileID);

    return std::move(drawable);
}

void RenderTerrain::activateLayerGroup(bool activate, UniqueChangeRequestVec& changes) {
    if (layerGroup) {
        if (activate) {
            changes.emplace_back(std::make_unique<AddLayerGroupRequest>(layerGroup));
        } else {
            changes.emplace_back(std::make_unique<RemoveLayerGroupRequest>(layerGroup));
        }
    }
}

void RenderTerrain::deactivate(UniqueChangeRequestVec& changes) {
    // depthLayerGroup / depthRenderTarget are owned by this RenderTerrain and
    // released with it; only the mesh layerGroup is registered separately with
    // the orchestrator, so that is all we need to unregister here.
    activateLayerGroup(false, changes);
}

#if MLN_RENDER_BACKEND_OPENGL
void RenderTerrain::packDEMArrayLayer(gfx::Context& context, const UnwrappedTileID& id, const DEMData& demData) {
    const auto& imagePtr = demData.getImagePtr();
    if (!imagePtr || imagePtr->size.isEmpty()) {
        return;
    }
    if (!demTextureArray) {
        demTextureArray = std::make_unique<gl::Texture2DArray>(static_cast<gl::Context&>(context));
    }
    // All DEM tiles from one source share a size, so this allocates once and no-ops after.
    demTextureArray->allocate(imagePtr->size, maxDEMArrayLayers);
    if (!demTextureArray->valid()) {
        return;
    }

    uint32_t layer = 0;
    if (const auto it = demArrayLayer.find(id); it != demArrayLayer.end()) {
        layer = it->second; // re-upload into the tile's existing slot
    } else if (!demArrayFreeLayers.empty()) {
        layer = demArrayFreeLayers.back();
        demArrayFreeLayers.pop_back();
        demArrayLayer[id] = layer;
    } else if (demArrayNextLayer < maxDEMArrayLayers) {
        layer = demArrayNextLayer++;
        demArrayLayer[id] = layer;
    } else {
        return; // array full - tile keeps its per-tile texture, just not instanced
    }
    demTextureArray->uploadLayer(layer, imagePtr->data.get());
}

void RenderTerrain::freeDEMArrayLayer(const UnwrappedTileID& id) {
    if (const auto it = demArrayLayer.find(id); it != demArrayLayer.end()) {
        demArrayFreeLayers.push_back(it->second);
        demArrayLayer.erase(it);
    }
}

// Build the single instanced depth drawable covering the current depthInstances: the shared
// depth mesh drawn N times, with a_instance = [0..N-1] selecting each tile's slot in the
// TerrainDepthInstanceUBO array (filled per frame in updateInstancedDepthUBO). No tile id is
// set, so the terrain tweaker skips it; no gfx textures, since the DEM array is bound manually
// in renderDepth. Called only when the tile set changes.
void RenderTerrain::rebuildInstancedDepthDrawable(gfx::Context& context, gfx::ShaderRegistry& shaders) {
    auto* depthLg = static_cast<LayerGroup*>(depthLayerGroup.get());
    if (!depthLg) {
        return;
    }
    depthLg->clearDrawables();
    const std::size_t n = depthInstances.size();
    if (n == 0 || !demTextureArray || !demTextureArray->valid()) {
        return;
    }

    const auto& depthMeshRef = getDepthMesh(context);
    if (depthMeshRef.vertices.empty() || depthMeshRef.indices.empty()) {
        return;
    }
    auto shader = context.getGenericShader(shaders, "TerrainDepthShader");
    if (!shader) {
        return;
    }
    auto builder = context.createDrawableBuilder("terrain-depth-instanced");
    if (!builder) {
        return;
    }
    builder->setShader(shader);
    builder->setRenderPass(RenderPass::Translucent);
    builder->setDepthType(gfx::DepthMaskType::ReadWrite);
    builder->setColorMode(gfx::ColorMode::unblended());
    builder->setEnableDepth(true);
    builder->setIs3D(true);

    std::vector<uint8_t> vtx(depthMeshRef.vertices.size() * sizeof(int16_t));
    std::memcpy(vtx.data(), depthMeshRef.vertices.data(), vtx.size());
    builder->setRawVertices(std::move(vtx), depthMeshRef.vertexCount, gfx::AttributeDataType::Short4);

    SegmentVector segs;
    segs.emplace_back(0, 0, depthMeshRef.vertexCount, depthMeshRef.indexCount);
    std::vector<uint16_t> idx = depthMeshRef.indices;
    builder->setSegments(gfx::Triangles(), std::move(idx), segs.data(), segs.size());

    // Per-instance index attribute (divisor 1). Its element count is the instance count the
    // GL backend draws (drawInstanced uses instanceAttrs->getMinCount()).
    auto instAttrs = context.createVertexAttributeArray();
    if (const auto& a = instAttrs->set(shaders::idTerrainInstanceVertexAttribute)) {
        for (std::size_t i = 0; i < n; ++i) {
            a->set(i, static_cast<float>(i));
        }
    }
    builder->setInstanceAttributes(std::move(instAttrs));

    builder->flush(context);
    auto drawables = builder->clearDrawables();
    if (!drawables.empty()) {
        // Bind the packed DEM array as u_dem_array (slot idTerrainDEMArrayTexture); DrawableGL
        // binds it in bindTextures() with the program active, using the shader sampler location.
        static_cast<gl::DrawableGL&>(*drawables[0])
            .setArrayTexture(demTextureArray.get(), shaders::idTerrainDEMArrayTexture);
        depthLg->addDrawable(std::move(drawables[0]));
    }
}

// Refresh the per-instance UBO array every frame (the transform depends on the camera) and
// bind it + the shared props UBO directly on the instanced drawable, so the terrain tweaker's
// layer-level TerrainDrawableUBO consolidation does not clobber it. Bound at idTerrainDrawableUBO
// as the array the shader indexes by a_instance.
void RenderTerrain::updateInstancedDepthUBO(PaintParameters& parameters) {
    auto* depthLg = static_cast<LayerGroup*>(depthLayerGroup.get());
    const std::size_t n = depthInstances.size();

    if (!depthLg || n == 0) {
        return;
    }
    auto& context = parameters.context;

    // The shader declares the block as a fixed array u_inst[TERRAIN_MAX_INSTANCES]
    // (== maxDepthInstances), so GLES requires the bound buffer/range to be at least
    // that full static size (sizeof(UBO) * maxDepthInstances). Allocate the whole block
    // and fill only the first n entries; the rest stay zero-initialized. Sizing the
    // buffer to n instead triggers "Bound buffer is too small" and the draw is dropped.
    std::vector<shaders::TerrainDepthInstanceUBO> arr(maxDepthInstances);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& inst = depthInstances[i];
        mat4 m = parameters.matrixForTile(inst.tileID.toUnwrapped());
#if !MLN_RENDER_BACKEND_OPENGL
        matrix::remapClipZToUnitRange(m);
#endif
        arr[i].matrix = util::cast<float>(m);
        arr[i].dem_coords = inst.demCoords;
        arr[i].dem_layer = inst.demLayer;
        arr[i].pad1 = arr[i].pad2 = arr[i].pad3 = 0.0f;
    }
    const std::size_t bytes = sizeof(shaders::TerrainDepthInstanceUBO) * maxDepthInstances;
    if (!depthInstanceUBO || depthInstanceUBO->getSize() < bytes) {
        depthInstanceUBO = context.createUniformBuffer(arr.data(), bytes, false, true);
    } else {
        depthInstanceUBO->update(arr.data(), bytes);
    }

    // Shared evaluated props (unpack / exaggeration / skirt offset), same as the tweaker.
    const auto zoom = std::max(parameters.state.getZoom(), 0.0);
    const float elevationOffset = static_cast<float>(util::M2PI * util::EARTH_RADIUS_M / std::pow(2.0, zoom) / 5.0);
    const shaders::TerrainEvaluatedPropsUBO propsUBO = {.unpack = getDEMUnpackVector(),
                                                        .exaggeration = getExaggeration(),
                                                        .elevation_offset = elevationOffset,
                                                        .pad1 = 0.0f,
                                                        .pad2 = 0.0f};

    depthLg->visitDrawables([&](gfx::Drawable& drawable) {
        auto& u = drawable.mutableUniformBuffers();
        u.set(shaders::idTerrainDrawableUBO, depthInstanceUBO);
        u.createOrUpdate(shaders::idTerrainEvaluatedPropsUBO, &propsUBO, context);
    });
}
#endif

} // namespace mln
