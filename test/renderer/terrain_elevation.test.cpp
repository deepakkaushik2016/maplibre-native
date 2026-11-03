#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
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

// A z10 DEM whose texel (i, j) is 500 + 2 i + 3 j metres: a plane, so the bilinear sample
// between texels is exact, and steep enough (2-3 m per texel) that sampling the wrong texel,
// or half a texel off, misses the 0.5 m tolerance.
constexpr int demZoom = 10;
constexpr uint32_t demDim = 256;
constexpr int knownI = 100;
constexpr int knownJ = 60;
constexpr double knownMeters = 500.0 + 2.0 * knownI + 3.0 * knownJ; // 880 m

double demMeters(uint32_t i, uint32_t j) {
    return 500.0 + 2.0 * i + 3.0 * j;
}

std::string gradientTile() {
    PremultipliedImage image({demDim, demDim});
    for (uint32_t j = 0; j < demDim; ++j) {
        for (uint32_t i = 0; i < demDim; ++i) {
            // Terrain-RGB: metres = -10000 + (R * 65536 + G * 256 + B) * 0.1.
            const auto v = static_cast<uint32_t>(std::lround((demMeters(i, j) + 10000.0) * 10.0));
            uint8_t* px = image.data.get() + (j * demDim + i) * 4;
            px[0] = static_cast<uint8_t>((v >> 16) & 0xFF);
            px[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
            px[2] = static_cast<uint8_t>(v & 0xFF);
            px[3] = 255;
        }
    }
    return encodePNG(image);
}

// Where texel (knownI, knownJ) of the z10 tile holding (46, 10) sits. A DEM texel i is sampled
// at tile x = i / dim (maplibre-gl-js Terrain.getDEMElevation's convention, which
// RenderTerrain::getElevation follows), so that is where the known value is exact.
LatLng knownPoint() {
    const double n = std::pow(2.0, demZoom);
    const double lat0 = 46.0 * M_PI / 180.0;
    const double tileX = std::floor((10.0 + 180.0) / 360.0 * n);
    const double tileY = std::floor((1.0 - std::log(std::tan(lat0) + 1.0 / std::cos(lat0)) / M_PI) / 2.0 * n);
    const double x = tileX + static_cast<double>(knownI) / demDim;
    const double y = tileY + static_cast<double>(knownJ) / demDim;
    const double lng = x / n * 360.0 - 180.0;
    const double lat = std::atan(std::sinh(M_PI * (1.0 - 2.0 * y / n))) * 180.0 / M_PI;
    return {lat, lng};
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
  },)STYLE" + terrain +
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
    std::string tile = gradientTile();
    int demRequests = 0;
    HeadlessFrontend frontend{{256, 256}, 1};
    MapAdapter map;

    // `deliverDEM` false holds every DEM response, so terrain is set but no DEM tile ever loads;
    // the map then runs continuously, as a still render would wait for the DEM forever.
    ElevationScene(const std::string& terrain, bool deliverDEM = true)
        : map(frontend,
              MapObserver::nullObserver(),
              fileSource,
              MapOptions()
                  .withMapMode(deliverDEM ? MapMode::Static : MapMode::Continuous)
                  .withSize(frontend.getSize())) {
        fileSource->tileResponse = [this, deliverDEM](const Resource&) -> std::optional<Response> {
            ++demRequests;
            if (!deliverDEM) {
                return std::nullopt;
            }
            Response res;
            res.data = std::make_shared<std::string>(tile);
            return res;
        };
        map.getStyle().loadJSON(demStyle(terrain));
        map.jumpTo(CameraOptions().withCenter(knownPoint()).withZoom(12.0));
        if (deliverDEM) {
            frontend.render(map);
        } else {
            for (int i = 0; i < 10; ++i) {
                frontend.renderOnce(map);
            }
        }
    }

    std::optional<double> query() { return frontend.getRenderer()->queryTerrainElevation(knownPoint()); }
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
    enum class Phase { Deliver, Hold, Wipe };
    Phase phase = Phase::Deliver;
    int wipes = 0;

    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    const std::string tile = gradientTile();
    fileSource->tileResponse = [&](const Resource&) -> std::optional<Response> {
        if (phase == Phase::Hold) {
            return std::nullopt;
        }
        Response res;
        if (phase == Phase::Deliver) {
            res.data = std::make_shared<std::string>(tile);
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
    const auto query = [&] { return frontend.getRenderer()->queryTerrainElevation(knownPoint()); };

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
