#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/renderer/update_parameters.hpp>
#include <mln/storage/resource.hpp>
#include <mln/storage/response.hpp>
#include <mln/style/source_impl.hpp>
#include <mln/style/style.hpp>
#include <mln/util/image.hpp>
#include <mln/util/chrono.hpp>
#include <mln/util/run_loop.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace mln;

// The renderer keeps its RenderTerrain for as long as the update's terrain is the same, but the
// RenderSource of the terrain's DEM can be destroyed under it: an update that leaves the DEM source
// out (Style::removeSource allowed that until it learned to refuse), one whose source list has it in
// another place among the others, which the diff reads as a removal and an addition (Style keeps its
// sources sorted by ID, but a renderer takes updates from any frontend), and Renderer::clearData,
// which drops every source. The terrain must then let go of the old source and draw, and answer
// queries, from the new one. Holding on to the old one read freed memory (heap-use-after-free under
// AddressSanitizer; a crash in DEMElevationProvider, on the next frame, otherwise). The tests also
// check the elevation the terrain reports, which it reads from the DEM source it holds: the new
// source is served a different height, so a terrain left on the old one shows even without
// AddressSanitizer.

namespace {

const LatLng center{46.0, 10.0};

// A Terrain-RGB tile of one height: -10000 + (R * 65536 + G * 256 + B) * 0.1 metres.
std::string plateauTile(uint8_t r, uint8_t g, uint8_t b) {
    PremultipliedImage image({256, 256});
    for (uint32_t i = 0; i < 256 * 256; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        px[0] = r;
        px[1] = g;
        px[2] = b;
        px[3] = 255;
    }
    return encodePNG(image);
}

std::string areaSource(const std::string& id, double west) {
    const std::string w = std::to_string(west);
    const std::string e = std::to_string(west + 0.5);
    return R"(")" + id + R"(": {"type": "geojson", "data": {"type": "Feature", "properties": {}, "geometry":
        {"type": "Polygon", "coordinates": [[[)" +
           w + ", 45.7], [" + e + ", 45.7], [" + e + ", 46.3], [" + w + ", 46.3], [" + w + R"(, 45.7]]]}}})";
}

// A DEM source for the terrain, and two sources that layers use.
std::string styleJSON() {
    return R"STYLE({
  "version": 8,
  "sources": {
    "dem": {"type": "raster-dem", "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"], "encoding": "mapbox",
            "maxzoom": 10, "tileSize": 256},
    )STYLE" +
           areaSource("west", 9.5) + ",\n    " + areaSource("east", 10.0) + R"STYLE(
  },
  "terrain": {"source": "dem", "exaggeration": 1},
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "white"}},
    {"id": "west", "type": "fill", "source": "west", "paint": {"fill-color": "#3a7"}},
    {"id": "east", "type": "fill", "source": "east", "paint": {"fill-color": "#a37"}}
  ]
})STYLE";
}

// Keeps the last update the map sent, so that a test can send the renderer a variation of it.
class CapturingFrontend : public HeadlessFrontend {
public:
    using HeadlessFrontend::HeadlessFrontend;
    void update(std::shared_ptr<UpdateParameters> parameters) override {
        last = parameters;
        HeadlessFrontend::update(std::move(parameters));
    }
    std::shared_ptr<UpdateParameters> last;
};

using SourceImpls = std::vector<Immutable<style::Source::Impl>>;

// `p` with another source list.
std::shared_ptr<UpdateParameters> withSources(const UpdateParameters& p, SourceImpls sources) {
    return std::make_shared<UpdateParameters>(
        UpdateParameters{.styleLoaded = p.styleLoaded,
                         .mode = p.mode,
                         .pixelRatio = p.pixelRatio,
                         .debugOptions = p.debugOptions,
                         .timePoint = p.timePoint,
                         .transformState = p.transformState,
                         .glyphURL = p.glyphURL,
                         .fontFaces = p.fontFaces,
                         .spriteLoaded = p.spriteLoaded,
                         .transitionOptions = p.transitionOptions,
                         .light = p.light,
                         .sky = p.sky,
                         .terrain = p.terrain,
                         .images = p.images,
                         .sources = makeMutable<SourceImpls>(std::move(sources)),
                         .layers = p.layers,
                         .globalState = p.globalState,
                         .annotationManager = p.annotationManager,
                         .fileSource = p.fileSource,
                         .prefetchZoomDelta = p.prefetchZoomDelta,
                         .stillImageRequest = p.stillImageRequest,
                         .crossSourceCollisions = p.crossSourceCollisions,
                         .fastPFOREnabled = p.fastPFOREnabled,
                         .captureRenderedFeatures = p.captureRenderedFeatures,
                         .tileLodMinRadius = p.tileLodMinRadius,
                         .tileLodScale = p.tileLodScale,
                         .tileLodPitchThreshold = p.tileLodPitchThreshold,
                         .tileLodZoomShift = p.tileLodZoomShift,
                         .tileLodMode = p.tileLodMode,
                         .terrainLoadMode = p.terrainLoadMode,
                         .terrainSkirtLength = p.terrainSkirtLength,
                         .debugAboveGroundLog = p.debugAboveGroundLog,
                         .transitionTargetCenter = p.transitionTargetCenter,
                         .reportCenterElevationOnMove = p.reportCenterElevationOnMove,
                         .terrainGrab = p.terrainGrab});
}

struct Scene {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    // What the DEM tiles are served as from now on: 1000 m, then 2000 m.
    std::string demTile = plateauTile(1, 173, 176);
    CapturingFrontend frontend{{256, 256}, 1};
    MapAdapter map{frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())};

    Scene() {
        fileSource->tileResponse = [this](const Resource&) {
            Response res;
            res.data = std::make_shared<std::string>(demTile);
            return res;
        };
        map.getStyle().loadJSON(styleJSON());
        map.jumpTo(CameraOptions().withCenter(center).withZoom(10.0).withPitch(45.0));
    }

    void serveHigherDEM() { demTile = plateauTile(1, 212, 192); }

    // One still frame, with every tile it needs.
    void render() { frontend.render(map); }

    std::optional<double> elevation() { return frontend.getRenderer()->queryTerrainElevation(center); }

    // Sends the renderer `parameters` in place of the map's update, and renders frames until the
    // terrain reports `expected` (or none, when empty), or ten seconds have passed. Returns the
    // last report.
    std::optional<double> renderUntil(std::shared_ptr<UpdateParameters> parameters, std::optional<double> expected) {
        frontend.update(std::move(parameters));
        const auto deadline = Clock::now() + Seconds(10);
        std::optional<double> reported;
        do {
            loop.runOnce();
            frontend.renderFrame();
            reported = elevation();
        } while (Clock::now() < deadline &&
                 (reported.has_value() != expected.has_value() || (reported && std::abs(*reported - *expected) > 1.0)));
        return reported;
    }
};

} // namespace

TEST(TerrainDEMSourceLifetime, KeptTerrainLetsGoOfTheDEMSourcesAnUpdateDestroys) {
    Scene scene;
    scene.render();
    const auto before = scene.elevation();
    ASSERT_TRUE(before) << "the DEM never loaded";
    ASSERT_NEAR(*before, 1000.0, 1.0);
    ASSERT_TRUE(scene.frontend.last);
    const UpdateParameters original = *scene.frontend.last;
    // Sorted by ID, as Style keeps them: the annotations' source, "dem", "east", "west".
    SourceImpls others;
    std::optional<Immutable<style::Source::Impl>> dem;
    for (const auto& source : *original.sources) {
        if (source->id == "dem") {
            dem = source;
        } else {
            others.push_back(source);
        }
    }
    ASSERT_TRUE(dem);
    ASSERT_GE(others.size(), 2u);
    ASSERT_NE(original.sources->back()->id, "dem");

    // The same update, terrain included, with the DEM source moved after the others: the diff keeps
    // them, and destroys and recreates "dem", whose new tiles are 2000 m high.
    scene.serveHigherDEM();
    SourceImpls moved = others;
    moved.push_back(*dem);
    auto reported = scene.renderUntil(withSources(original, moved), 2000.0);
    ASSERT_TRUE(reported) << "the terrain lost its DEM";
    EXPECT_NEAR(*reported, 2000.0, 1.0) << "the terrain still reads the DEM source the update destroyed";

    // Without the DEM source, under the same terrain: nothing to report, and nothing freed to read.
    reported = scene.renderUntil(withSources(original, others), std::nullopt);
    EXPECT_EQ(reported, std::nullopt) << "the terrain still reads the DEM source the update removed";

    // And back as the map has it.
    reported = scene.renderUntil(withSources(original, *original.sources), 2000.0);
    ASSERT_TRUE(reported) << "the terrain did not pick up the DEM source again";
    EXPECT_NEAR(*reported, 2000.0, 1.0);
}

TEST(TerrainDEMSourceLifetime, KeptTerrainFollowsTheSourcesClearDataRecreates) {
    Scene scene;
    scene.render();
    const auto before = scene.elevation();
    ASSERT_TRUE(before) << "the DEM never loaded";
    ASSERT_NEAR(*before, 1000.0, 1.0);

    // Drops every render source but not the terrain, which the style still has. Until a frame makes
    // the sources again, the terrain has no DEM to answer from.
    scene.frontend.getRenderer()->clearData();
    EXPECT_EQ(scene.elevation(), std::nullopt);

    scene.serveHigherDEM();
    scene.render();
    const auto after = scene.elevation();
    ASSERT_TRUE(after) << "the terrain lost its DEM";
    EXPECT_NEAR(*after, 2000.0, 1.0) << "the terrain still reads the DEM source clearData destroyed";
}
