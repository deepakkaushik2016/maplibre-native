#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>
#include <mln/test/renderer_test_access.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/style/style.hpp>
#include <mln/style/terrain.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>

#include <cmath>
#include <memory>
#include <optional>
#include <string>

using namespace mln;

// Replacing the terrain at runtime (an exaggeration slider, a 3D toggle) drops the RenderTerrain
// and builds a new one. The old mesh layer group must be unregistered from the orchestrator on
// every path: the orchestrator keys its layer groups in a multimap, so a group that is not removed
// keeps drawing next to the new one (TERRAIN.md, "replacing terrain at runtime orphaned the old
// mesh"). These tests replace the terrain many times and check that neither the number of terrain
// mesh groups nor the frame's draw calls grow.

namespace {

constexpr int iterations = 100;
constexpr double plateauMeters = 1000.0;
const LatLng center{46.0, 10.0};
// RenderTerrain's mesh layer group (RenderTerrain::update creates it under this name).
const std::string terrainGroup = "terrain";

std::string plateauTile() {
    // Terrain-RGB: -10000 + (R * 65536 + G * 256 + B) * 0.1 = 1000 m for RGB(1, 173, 176).
    PremultipliedImage image({256, 256});
    for (uint32_t i = 0; i < 256 * 256; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        px[0] = 1;
        px[1] = 173;
        px[2] = 176;
        px[3] = 255;
    }
    return encodePNG(image);
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
    },
    "area": {
      "type": "geojson",
      "data": {
        "type": "Feature",
        "properties": {},
        "geometry": {"type": "Polygon", "coordinates": [[[9.5, 45.7], [10.5, 45.7], [10.5, 46.3], [9.5, 46.3], [9.5, 45.7]]]}
      }
    }
  },)STYLE" +
           terrain +
           R"STYLE(
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "white"}},
    {"id": "area", "type": "fill", "source": "area", "paint": {"fill-color": "#3a7", "fill-opacity": 0.5}}
  ]
})STYLE";
}

struct ReplaceScene {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    std::string tile = plateauTile();
    HeadlessFrontend frontend{{256, 256}, 1};
    MapAdapter map{frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())};

    explicit ReplaceScene(const std::string& terrain) {
        fileSource->tileResponse = [this](const Resource&) {
            Response res;
            res.data = std::make_shared<std::string>(tile);
            return res;
        };
        map.getStyle().loadJSON(demStyle(terrain));
        // z10: the camera is high enough above the plateau at every exaggeration used that the
        // frame's draw calls do not depend on it, so any change in them is a leak. From about
        // z13 the count swings with exaggeration (the camera is not anchored to the terrain).
        map.jumpTo(CameraOptions().withCenter(center).withZoom(10.0));
    }

    struct Frame {
        int drawCalls;
        // Pixels other than the white background: the area polygon covers the whole view, so a
        // frame that draws it has ink everywhere.
        int inked;
    };

    // One still frame: waits for every tile it needs, so each call is a complete, settled frame.
    Frame render() {
        const auto result = frontend.render(map);
        int inked = 0;
        const PremultipliedImage& image = result.image;
        for (size_t i = 0; i < image.bytes(); i += 4) {
            const uint8_t* px = image.data.get() + i;
            inked += px[0] != 255 || px[1] != 255 || px[2] != 255 ? 1 : 0;
        }
        return {.drawCalls = result.stats.numDrawCalls, .inked = inked};
    }

    size_t terrainGroups() { return RendererTestAccess::layerGroupCount(*frontend.getRenderer(), terrainGroup); }

    std::optional<double> elevation() { return frontend.getRenderer()->queryTerrainElevation(center); }
};

} // namespace

TEST(TerrainReplace, ExaggerationSweepKeepsOneTerrainGroup) {
    ReplaceScene scene(R"("terrain": {"source": "dem", "exaggeration": 1},)");
    const auto firstFrame = scene.render();
    ASSERT_GT(firstFrame.inked, 0) << "the first frame is blank";
    const int first = firstFrame.drawCalls;
    ASSERT_EQ(scene.terrainGroups(), 1u);
    ASSERT_GT(first, 0);
    const auto firstElevation = scene.elevation();
    ASSERT_TRUE(firstElevation) << "the DEM never loaded";
    ASSERT_NEAR(*firstElevation, plateauMeters, 1.0);

    const int tolerance = static_cast<int>(std::ceil(0.05 * first));
    for (int i = 0; i < iterations; ++i) {
        const float exaggeration = i % 2 == 0 ? 2.0f : 0.5f;
        scene.map.getStyle().getTerrain()->setExaggeration(exaggeration);
        const auto frame = scene.render();
        ASSERT_GT(frame.inked, 0) << "iteration " << i << " (exaggeration " << exaggeration << "): blank frame";
        const int calls = frame.drawCalls;

        ASSERT_EQ(scene.terrainGroups(), 1u) << "iteration " << i << " (exaggeration " << exaggeration
                                             << "): a replaced terrain left its mesh group behind";
        ASSERT_NEAR(calls, first, tolerance) << "iteration " << i << " (exaggeration " << exaggeration
                                             << "): draw calls moved from the first frame's " << first;
        // The replacement really happened: the surface is drawn at the new height.
        const auto elevation = scene.elevation();
        ASSERT_TRUE(elevation) << "iteration " << i;
        ASSERT_NEAR(*elevation, plateauMeters * exaggeration, 1.0) << "iteration " << i;
    }
}

TEST(TerrainReplace, ToggleOnOffReturnsToBaseline) {
    ReplaceScene scene("");
    const auto baselineFrame = scene.render();
    ASSERT_GT(baselineFrame.inked, 0) << "the frame without terrain is blank";
    const int baseline = baselineFrame.drawCalls;
    ASSERT_EQ(scene.terrainGroups(), 0u);
    ASSERT_EQ(scene.elevation(), std::nullopt);

    std::optional<int> firstOn;
    int lastOff = -1;
    for (int i = 0; i < iterations; ++i) {
        scene.map.getStyle().setTerrain(std::make_unique<style::Terrain>("dem", 1.25f));
        const auto onFrame = scene.render();
        ASSERT_GT(onFrame.inked, 0) << "cycle " << i << " (on): blank frame";
        const int on = onFrame.drawCalls;
        ASSERT_EQ(scene.terrainGroups(), 1u) << "cycle " << i << " (on)";
        const auto elevation = scene.elevation();
        ASSERT_TRUE(elevation) << "cycle " << i << ": terrain on but no elevation";
        ASSERT_NEAR(*elevation, plateauMeters * 1.25, 1.0) << "cycle " << i;
        if (!firstOn) {
            firstOn = on;
            ASSERT_GT(on, baseline) << "terrain on draws nothing more than terrain off";
        }
        ASSERT_EQ(on, *firstOn) << "cycle " << i << " (on): draw calls grew";

        scene.map.getStyle().setTerrain(nullptr);
        const auto offFrame = scene.render();
        ASSERT_GT(offFrame.inked, 0) << "cycle " << i << " (off): blank frame";
        lastOff = offFrame.drawCalls;
        ASSERT_EQ(scene.terrainGroups(), 0u) << "cycle " << i << " (off): a terrain mesh group is still drawn";
        ASSERT_EQ(lastOff, baseline) << "cycle " << i << " (off)";
    }
    EXPECT_EQ(lastOff, baseline);
}
