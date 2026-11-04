#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/geometry/dem_data.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/renderer/render_terrain.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/style/style.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace mln;

namespace {
constexpr float extent = static_cast<float>(util::EXTENT);

// z2 has a 4x4 tile grid, small enough to reason about the edges by hand.
UnwrappedTileID z2(uint32_t x, uint32_t y, int16_t wrap = 0) {
    return UnwrappedTileID(wrap, CanonicalTileID(2, x, y));
}
} // namespace

TEST(TerrainElevation, NormalizeLeavesInRangeCoordinatesAlone) {
    UnwrappedTileID tile = z2(1, 1);
    float x = 10.0f;
    float y = extent - 1.0f;

    EXPECT_TRUE(RenderTerrain::normalizeTileCoordinates(tile, x, y));
    EXPECT_EQ(tile, z2(1, 1));
    EXPECT_FLOAT_EQ(x, 10.0f);
    EXPECT_FLOAT_EQ(y, extent - 1.0f);
}

TEST(TerrainElevation, NormalizeCrossesTileEdges) {
    // East, past the right edge.
    UnwrappedTileID tile = z2(1, 1);
    float x = extent + 100.0f;
    float y = 50.0f;
    EXPECT_TRUE(RenderTerrain::normalizeTileCoordinates(tile, x, y));
    EXPECT_EQ(tile, z2(2, 1));
    EXPECT_FLOAT_EQ(x, 100.0f);
    EXPECT_FLOAT_EQ(y, 50.0f);

    // West, before the left edge.
    tile = z2(1, 1);
    x = -100.0f;
    y = 50.0f;
    EXPECT_TRUE(RenderTerrain::normalizeTileCoordinates(tile, x, y));
    EXPECT_EQ(tile, z2(0, 1));
    EXPECT_FLOAT_EQ(x, extent - 100.0f);

    // North, above the top edge.
    tile = z2(1, 1);
    x = 50.0f;
    y = -100.0f;
    EXPECT_TRUE(RenderTerrain::normalizeTileCoordinates(tile, x, y));
    EXPECT_EQ(tile, z2(1, 0));
    EXPECT_FLOAT_EQ(y, extent - 100.0f);

    // South, below the bottom edge.
    tile = z2(1, 1);
    x = 50.0f;
    y = extent + 100.0f;
    EXPECT_TRUE(RenderTerrain::normalizeTileCoordinates(tile, x, y));
    EXPECT_EQ(tile, z2(1, 2));
    EXPECT_FLOAT_EQ(y, 100.0f);
}

TEST(TerrainElevation, NormalizeCrossesMoreThanOneTile) {
    UnwrappedTileID tile = z2(1, 1);
    float x = 2.5f * extent;
    float y = 50.0f;

    EXPECT_TRUE(RenderTerrain::normalizeTileCoordinates(tile, x, y));
    EXPECT_EQ(tile, z2(3, 1));
    EXPECT_FLOAT_EQ(x, 0.5f * extent);
}

TEST(TerrainElevation, NormalizeWrapsAroundTheAntimeridian) {
    // Off the east edge of the world: same canonical column 0, one wrap further east.
    UnwrappedTileID tile = z2(3, 1);
    float x = extent + 50.0f;
    float y = 50.0f;
    EXPECT_TRUE(RenderTerrain::normalizeTileCoordinates(tile, x, y));
    EXPECT_EQ(tile, z2(0, 1, /*wrap=*/1));
    EXPECT_FLOAT_EQ(x, 50.0f);

    // Off the west edge, the other way.
    tile = z2(0, 1);
    x = -50.0f;
    y = 50.0f;
    EXPECT_TRUE(RenderTerrain::normalizeTileCoordinates(tile, x, y));
    EXPECT_EQ(tile, z2(3, 1, /*wrap=*/-1));
    EXPECT_FLOAT_EQ(x, extent - 50.0f);
}

TEST(TerrainElevation, NormalizeRejectsPastThePoles) {
    // North of the top row: the grid does not continue, so there is nothing to sample. The
    // inputs are left untouched so a caller can tell nothing was resolved.
    UnwrappedTileID tile = z2(1, 0);
    float x = 50.0f;
    float y = -50.0f;
    EXPECT_FALSE(RenderTerrain::normalizeTileCoordinates(tile, x, y));
    EXPECT_EQ(tile, z2(1, 0));
    EXPECT_FLOAT_EQ(y, -50.0f);

    // South of the bottom row.
    tile = z2(1, 3);
    x = 50.0f;
    y = extent + 50.0f;
    EXPECT_FALSE(RenderTerrain::normalizeTileCoordinates(tile, x, y));
    EXPECT_EQ(tile, z2(1, 3));
}

TEST(TerrainElevation, NormalizeKeepsTheResultInsideTheTile) {
    // A coordinate a hair below zero must not round back up onto the far edge, which belongs
    // to the next tile over.
    UnwrappedTileID tile = z2(1, 1);
    float x = -1e-9f;
    float y = 50.0f;

    EXPECT_TRUE(RenderTerrain::normalizeTileCoordinates(tile, x, y));
    EXPECT_EQ(tile, z2(0, 1));
    EXPECT_GE(x, 0.0f);
    EXPECT_LT(x, extent);
}

namespace {

// A z10 DEM that is one plane across its tiles: DEM pixel (i, j), counted from the first pixel of
// the tile holding (46, 10), is 500 + 2 i + 3 j metres. Bilinear interpolation between pixels
// reproduces a plane exactly, and it is steep enough (2-3 m per pixel) that sampling the wrong
// pixel, or half a pixel off, misses by 2.5 m or more.
constexpr int demZoom = 10;
constexpr uint32_t demDim = 256;
constexpr int knownI = 100;
constexpr int knownJ = 60;
constexpr double knownMeters = 500.0 + 2.0 * knownI + 3.0 * knownJ; // 880 m

double demMeters(double i, double j) {
    return 500.0 + 2.0 * i + 3.0 * j;
}

// The z10 tile holding (46, 10), whose first pixel the plane counts from.
CanonicalTileID knownTile() {
    const double n = std::pow(2.0, demZoom);
    const double lat0 = 46.0 * M_PI / 180.0;
    const double tileX = std::floor((10.0 + 180.0) / 360.0 * n);
    const double tileY = std::floor((1.0 - std::log(std::tan(lat0) + 1.0 / std::cos(lat0)) / M_PI) / 2.0 * n);
    return {static_cast<uint8_t>(demZoom), static_cast<uint32_t>(tileX), static_cast<uint32_t>(tileY)};
}

// The point `u`, `v` DEM pixels right of and below the known tile's top-left corner. DEM pixel i
// covers [i, i + 1), so its centre is at i + 0.5.
LatLng demPoint(double u, double v) {
    const double n = std::pow(2.0, demZoom);
    const CanonicalTileID tile = knownTile();
    const double x = tile.x + u / demDim;
    const double y = tile.y + v / demDim;
    const double lng = x / n * 360.0 - 180.0;
    const double lat = std::atan(std::sinh(M_PI * (1.0 - 2.0 * y / n))) * 180.0 / M_PI;
    return {lat, lng};
}

// The height of the terrain at `demPoint(u, v)`, as maplibre-gl-js draws and queries it: each DEM
// pixel describes the cell centred at i + 0.5 (`DEM_CELL_CENTER_OFFSET` in terrain_coverage.ts,
// `uv * dim + 1.5` in get_elevation), and the surface is bilinear between those centres.
double drawnMeters(double u, double v) {
    return demMeters(u - 0.5, v - 0.5);
}

// The centre of DEM pixel (knownI, knownJ), where the terrain is at that pixel's height exactly.
LatLng knownPoint() {
    return demPoint(knownI + 0.5, knownJ + 0.5);
}

// A tile of the plane, at any zoom: each pixel holds the plane at its centre.
std::string gradientTile(const CanonicalTileID& id) {
    const CanonicalTileID known = knownTile();
    const double scale = std::pow(2.0, demZoom - id.z);
    PremultipliedImage image({demDim, demDim});
    for (uint32_t y = 0; y < demDim; ++y) {
        for (uint32_t x = 0; x < demDim; ++x) {
            // This pixel's centre, in z10 DEM pixels from the known tile's corner.
            const double u = (static_cast<double>(id.x) * demDim + x + 0.5) * scale - known.x * double{demDim};
            const double v = (static_cast<double>(id.y) * demDim + y + 0.5) * scale - known.y * double{demDim};
            // Terrain-RGB: metres = -10000 + (R * 65536 + G * 256 + B) * 0.1.
            const auto value = static_cast<uint32_t>(std::lround((drawnMeters(u, v) + 10000.0) * 10.0));
            uint8_t* px = image.data.get() + (static_cast<size_t>(y) * demDim + x) * 4;
            px[0] = static_cast<uint8_t>((value >> 16) & 0xFF);
            px[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            px[2] = static_cast<uint8_t>(value & 0xFF);
            px[3] = 255;
        }
    }
    return encodePNG(image);
}

CanonicalTileID tileOf(const Resource& resource) {
    const auto& t = *resource.tileData;
    return {static_cast<uint8_t>(t.z), static_cast<uint32_t>(t.x), static_cast<uint32_t>(t.y)};
}

std::string demStyle(const std::string& terrain) {
    return R"STYLE({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"],
      "encoding": "mapbox",
      "maxzoom": 10,
      "tileSize": 256
    }
  },)STYLE" +
           terrain +
           R"STYLE(
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "white"}}
  ]
})STYLE";
}

struct ElevationScene {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    int demRequests = 0;
    HeadlessFrontend frontend{{256, 256}, 1};
    MapAdapter map;

    // `deliverDEM` false holds every DEM response, so terrain is set but no DEM tile ever loads;
    // the map then runs continuously, as a still render would wait for the DEM forever.
    ElevationScene(const std::string& terrain, bool deliverDEM = true, const LatLng& center = knownPoint())
        : map(frontend,
              MapObserver::nullObserver(),
              fileSource,
              MapOptions()
                  .withMapMode(deliverDEM ? MapMode::Static : MapMode::Continuous)
                  .withSize(frontend.getSize())) {
        fileSource->tileResponse = [this, deliverDEM](const Resource& resource) -> std::optional<Response> {
            ++demRequests;
            if (!deliverDEM) {
                return std::nullopt;
            }
            Response res;
            res.data = std::make_shared<std::string>(gradientTile(tileOf(resource)));
            return res;
        };
        map.getStyle().loadJSON(demStyle(terrain));
        map.jumpTo(CameraOptions().withCenter(center).withZoom(12.0));
        if (deliverDEM) {
            frontend.render(map);
        } else {
            for (int i = 0; i < 10; ++i) {
                frontend.renderOnce(map);
            }
        }
    }

    std::optional<double> query(const LatLng& at = knownPoint()) {
        return frontend.getRenderer()->queryTerrainElevation(at);
    }
};

} // namespace

TEST(TerrainElevation, RendererQueryMatchesDEM) {
    ElevationScene scene(R"("terrain": {"source": "dem", "exaggeration": 1},)");
    const auto q = scene.query();
    ASSERT_TRUE(q) << "no elevation where a loaded DEM tile covers the point";
    EXPECT_NEAR(*q, knownMeters, 0.5);
}

TEST(TerrainElevation, RendererQueryAppliesExaggeration) {
    ElevationScene scene(R"("terrain": {"source": "dem", "exaggeration": 2},)");
    const auto q = scene.query();
    ASSERT_TRUE(q) << "no elevation where a loaded DEM tile covers the point";
    EXPECT_NEAR(*q, 2.0 * knownMeters, 1.0);
}

// The queried height is the terrain as maplibre-gl-js has it: each DEM pixel at its centre, and
// bilinear between centres, also across a DEM tile's edge into its neighbour. The camera looks at
// the known tile's top-left corner, so the three tiles meeting it there are loaded too and
// backfill its border.
TEST(TerrainElevation, QuerySamplesEachDEMPixelAtItsCentre) {
    ElevationScene scene(R"("terrain": {"source": "dem", "exaggeration": 1},)", true, demPoint(0.0, 0.0));
    struct Case {
        const char* what;
        double u;
        double v;
    };
    const Case cases[] = {
        {"a pixel's centre", knownI + 0.5, knownJ + 0.5},
        {"a pixel's corner", knownI, knownJ},
        {"inside a pixel", knownI + 0.3, knownJ + 0.85},
        {"the first half pixel of the tile, west", 0.2, 10.5},
        {"the last half pixel of the west neighbour", -0.2, 10.5},
        {"the first half pixel of the tile, north", 10.5, 0.3},
        {"the last half pixel of the north neighbour", 10.5, -0.3},
        {"the corner of four tiles", 0.0, 0.0},
        {"the last half pixel of the north-west neighbour", -0.4, -0.1},
    };
    for (const auto& c : cases) {
        const auto q = scene.query(demPoint(c.u, c.v));
        ASSERT_TRUE(q) << c.what << ": no elevation where a loaded DEM tile covers the point";
        EXPECT_NEAR(*q, drawnMeters(c.u, c.v), 0.05) << c.what << " (" << c.u << ", " << c.v << ")";
    }
}

TEST(TerrainElevation, RendererQueryNulloptBeforeDEMLoads) {
    ElevationScene scene(R"("terrain": {"source": "dem", "exaggeration": 1},)", /*deliverDEM=*/false);
    // Terrain is on and asked for its DEM, which never arrives.
    ASSERT_GT(scene.demRequests, 0);
    EXPECT_EQ(scene.query(), std::nullopt);
}

TEST(TerrainElevation, RendererQueryNulloptWhenTerrainOff) {
    ElevationScene scene("");
    EXPECT_EQ(scene.query(), std::nullopt);
}

TEST(TerrainElevation, RendererQueryNulloptWhenTileHasNoData) {
    // The frame's DEM render tile covers the point, but its tile has since been reloaded with no
    // content, which drops its DEM data. Until the next frame rebuilds the render tiles, that
    // render tile is still there with no data behind it: the point has no elevation, not 0 m.
    //
    // A loaded tile is fetched again when it goes from optional back to required: zooming out
    // past the DEM's maxzoom (with the z9 parent held back) keeps the z10 tile as an optional
    // fill-in, and zooming back in makes it required again.
    enum class Phase {
        Deliver,
        Hold,
        Wipe
    };
    Phase phase = Phase::Deliver;
    int wipes = 0;

    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    fileSource->tileResponse = [&](const Resource& resource) -> std::optional<Response> {
        if (phase == Phase::Hold) {
            return std::nullopt;
        }
        Response res;
        if (phase == Phase::Deliver) {
            res.data = std::make_shared<std::string>(gradientTile(tileOf(resource)));
        } else {
            res.noContent = true;
            ++wipes;
        }
        return res;
    };
    // Frames are drawn only when asked, so the reload can land between two of them.
    HeadlessFrontend frontend{{256, 256},
                              1,
                              gfx::HeadlessBackend::SwapBehaviour::NoFlush,
                              gfx::ContextMode::Unique,
                              std::nullopt,
                              /*invalidateOnUpdate=*/false};
    MapAdapter map{frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Continuous).withSize(frontend.getSize())};
    const auto pump = [&](bool draw) {
        for (int i = 0; i < 100; ++i) {
            loop.runOnce();
            if (draw) {
                frontend.renderFrame();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    const auto query = [&] {
        return frontend.getRenderer()->queryTerrainElevation(knownPoint());
    };

    map.getStyle().loadJSON(demStyle(R"("terrain": {"source": "dem", "exaggeration": 1},)"));
    map.jumpTo(CameraOptions().withCenter(knownPoint()).withZoom(12.0));
    pump(true);
    ASSERT_TRUE(query()) << "the DEM never loaded";
    ASSERT_NEAR(*query(), knownMeters, 0.5);

    phase = Phase::Hold;
    map.jumpTo(CameraOptions().withZoom(9.0));
    pump(true);
    ASSERT_TRUE(query()) << "the z10 DEM tile was not kept while its parent loads";

    // Back in: the z10 tile is required again and refetched; it answers with no content, but
    // no frame is drawn after that, so the render tile stays.
    phase = Phase::Wipe;
    map.jumpTo(CameraOptions().withZoom(12.0));
    loop.runOnce();
    frontend.renderFrame();
    pump(false);
    phase = Phase::Hold;
    ASSERT_GT(wipes, 0) << "the z10 tile was never refetched";

    EXPECT_EQ(query(), std::nullopt);
}

namespace {

// A plane across a z10 tile, held by its DEM descendants `dz` zooms deeper (128 px each): in their
// pixels, counted across the whole tile, the terrain is 500 + 2 u + 3 v metres, and each pixel
// holds the plane at its centre. The stitched DEM has 256 px across, so a stitched pixel spans
// 2^(dz - 1) descendant pixels and its centre falls on a descendant pixel corner: integer
// metres, which DEMData::get reads exactly. A stitched pixel taken from one descendant pixel
// instead of resampled is half a descendant pixel off, 2.5 m.
constexpr int32_t stitchDescendantDim = 128;
const CanonicalTileID stitchTile{10, 540, 360};

double stitchPlane(double u, double v) {
    return 500.0 + 2.0 * u + 3.0 * v;
}

void expectStitchedPlane(int dz) {
    const int32_t n = 1 << dz;
    std::vector<std::shared_ptr<DEMData>> dems;
    for (int32_t cy = 0; cy < n; ++cy) {
        for (int32_t cx = 0; cx < n; ++cx) {
            PremultipliedImage image({stitchDescendantDim, stitchDescendantDim});
            for (int32_t y = 0; y < stitchDescendantDim; ++y) {
                for (int32_t x = 0; x < stitchDescendantDim; ++x) {
                    const double meters = stitchPlane(cx * stitchDescendantDim + x + 0.5,
                                                      cy * stitchDescendantDim + y + 0.5);
                    const auto value = static_cast<uint32_t>(std::lround((meters + 10000.0) * 10.0));
                    uint8_t* px = image.data.get() + (static_cast<size_t>(y) * stitchDescendantDim + x) * 4;
                    px[0] = static_cast<uint8_t>((value >> 16) & 0xFF);
                    px[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
                    px[2] = static_cast<uint8_t>(value & 0xFF);
                    px[3] = 255;
                }
            }
            dems.push_back(std::make_shared<DEMData>(image, Tileset::RasterEncoding::Mapbox));
        }
    }
    // Each descendant's border is backfilled from its neighbours, as the DEM source does.
    for (int32_t cy = 0; cy < n; ++cy) {
        for (int32_t cx = 0; cx < n; ++cx) {
            for (int32_t dy = -1; dy <= 1; ++dy) {
                for (int32_t dx = -1; dx <= 1; ++dx) {
                    if ((dx || dy) && cx + dx >= 0 && cx + dx < n && cy + dy >= 0 && cy + dy < n) {
                        dems[cy * n + cx]->backfillBorder(*dems[(cy + dy) * n + cx + dx], dx, dy);
                    }
                }
            }
        }
    }
    std::vector<RenderTerrain::StitchSource> sources;
    for (int32_t cy = 0; cy < n; ++cy) {
        for (int32_t cx = 0; cx < n; ++cx) {
            sources.push_back(
                {.id = CanonicalTileID(
                     static_cast<uint8_t>(stitchTile.z + dz), (stitchTile.x << dz) + cx, (stitchTile.y << dz) + cy),
                 .dem = dems[cy * n + cx].get()});
        }
    }
    const auto stitched = RenderTerrain::stitchDEM(stitchTile, sources);
    ASSERT_EQ(stitched->dim, 2 * stitchDescendantDim);
    const RenderTerrain::TileElevation elevation(stitched, 1.0f, 0.0f, 0.0f, 1.0f);

    // Fractions of the tile: a spread across it, and either side of every seam between
    // descendants, all at least a stitched pixel inside the tile (its outer border only repeats
    // the outermost descendants here).
    std::vector<double> at;
    for (int k = 0; k < 17; ++k) {
        at.push_back((k + 0.37) / 17.0);
    }
    for (int32_t c = 1; c < n; ++c) {
        at.push_back((c - 0.3 / stitched->dim) / n);
        at.push_back((c + 0.3 / stitched->dim) / n);
    }
    const double across = static_cast<double>(n) * stitchDescendantDim;
    for (const double fx : at) {
        for (const double fy : at) {
            const float h = elevation.height(static_cast<float>(fx * extent), static_cast<float>(fy * extent));
            const double expected = stitchPlane(fx * across, fy * across);
            EXPECT_NEAR(h, expected, 0.05) << "dz " << dz << " at (" << fx << ", " << fy << ")";
        }
    }
}

} // namespace

// A tile with no DEM of its own or above it is lifted by a DEM stitched from its descendants.
// Two and three zooms deeper (128 px DEM tiles under 512 px tiles, or 64 px ones) each stitched
// pixel spans several descendant pixels; it must hold the descendants' surface at its own
// centre, so the stitched terrain is the descendants' terrain.
TEST(TerrainElevation, StitchedDEMResamplesDescendantsTwoZoomsDeeper) {
    expectStitchedPlane(2);
}

TEST(TerrainElevation, StitchedDEMResamplesDescendantsThreeZoomsDeeper) {
    expectStitchedPlane(3);
}
