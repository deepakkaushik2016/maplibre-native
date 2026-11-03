#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/map/transform_state.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/style/style.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/image.hpp>
#include <mln/util/projection.hpp>
#include <mln/util/run_loop.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

using namespace mln;

namespace {

// With terrain on, a line is draped on the elevated surface, so a screen point must be unprojected
// onto that surface (not onto sea level) before it is matched against the line's tile geometry.
//
// The scene is a flat 1000 m plateau, so where the line is drawn is known exactly: its coordinate
// projected at 1000 m. Where the line WOULD be drawn without terrain is the same coordinate
// projected at 0 m. The camera's centre altitude is set to the plateau (what gl-js does with its
// centre clamped to the ground), which keeps the eye well above the surface, and the render is
// checked so the query really runs against what was drawn.
constexpr double plateauMeters = 1000.0;
constexpr uint32_t viewSize = 512;
const LatLng center{46.0, 10.0};
// A point on the east-west line, north of the centre so it is not on the screen's centre row.
const LatLng onLine{46.01, 10.0};

const char* const style = R"STYLE({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"],
      "encoding": "mapbox",
      "maxzoom": 10,
      "tileSize": 256
    },
    "trail": {
      "type": "geojson",
      "data": {
        "type": "Feature",
        "properties": {"name": "trail"},
        "geometry": {"type": "LineString", "coordinates": [[9.97, 46.01], [10.03, 46.01]]}
      }
    }
  },
  "terrain": {"source": "dem", "exaggeration": 1},
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "white"}},
    {"id": "trail", "type": "line", "source": "trail",
     "paint": {"line-color": "#d00", "line-width": 6}}
  ]
})STYLE";

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

// Map::pixelForLatLng (screen pixels, y down - what queryRenderedFeatures takes), but for a point
// `meters` above sea level instead of on it.
ScreenCoordinate project(const TransformState& state, const LatLng& latLng, double meters) {
    // Same chain as TransformState::coordinatePointMatrix: world pixels -> clip -> viewport, with
    // the world z in metres (Camera::getWorldToCamera scales it to pixels).
    const Point<double> pt = Projection::project(latLng, state.getScale());
    const vec4 c = {{pt.x, pt.y, meters, 1}};
    vec4 p;
    matrix::transformMat4(p, c, state.getProjectionMatrix());
    const Size size = state.getSize();
    const double x = (p[0] / p[3] + 1) * size.width / 2;
    // TransformState works y-up (latLngToScreenCoordinate's y is (ndc.y + 1) * height / 2), and
    // Transform flips it for the public y-down screen space.
    const double yUp = (p[1] / p[3] + 1) * size.height / 2;
    return {x, size.height - yUp};
}

bool isRed(const PremultipliedImage& image, int x, int y) {
    const uint8_t* px = image.data.get() + (static_cast<size_t>(y) * image.size.width + x) * 4;
    return px[0] > 150 && px[1] < 100 && px[2] < 100;
}

// Red pixels within `radius` of p (the line is 6 px wide, so a hit has several).
int redAround(const PremultipliedImage& image, const ScreenCoordinate& p, int radius = 3) {
    int count = 0;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int x = static_cast<int>(std::lround(p.x)) + dx;
            const int y = static_cast<int>(std::lround(p.y)) + dy;
            if (x >= 0 && y >= 0 && x < int(image.size.width) && y < int(image.size.height)) {
                count += isRed(image, x, y) ? 1 : 0;
            }
        }
    }
    return count;
}

struct TerrainQueryScene {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    std::string tile = plateauTile();
    HeadlessFrontend frontend{{viewSize, viewSize}, 1};
    MapAdapter map{frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())};
    PremultipliedImage image;
    ScreenCoordinate elevated;
    ScreenCoordinate seaLevel;

    TerrainQueryScene() {
        fileSource->tileResponse = [this](const Resource&) {
            Response res;
            res.data = std::make_shared<std::string>(tile);
            return res;
        };
        map.getStyle().loadJSON(style);
        map.jumpTo(CameraOptions().withCenter(center).withZoom(13.0).withPitch(50.0).withCenterAltitude(plateauMeters));
        for (int i = 0; i < 4; ++i) {
            loop.runOnce();
            image = frontend.render(map).image;
        }
        const auto state = frontend.getTransformState();
        EXPECT_TRUE(state);
        if (state) {
            elevated = project(*state, onLine, plateauMeters);
            seaLevel = project(*state, onLine, 0.0);
        }
        std::printf("elevated (%.1f, %.1f), sea level (%.1f, %.1f), %.1f px apart; red around them %d / %d\n",
                    elevated.x,
                    elevated.y,
                    seaLevel.x,
                    seaLevel.y,
                    distance(),
                    redAround(image, elevated),
                    redAround(image, seaLevel));
    }

    double distance() const { return std::hypot(elevated.x - seaLevel.x, elevated.y - seaLevel.y); }

    // The frame really shows the draped line where the elevated projection says, and nothing
    // where the sea-level projection says; without this a query result says nothing.
    void expectRenderedScene() const {
        ASSERT_EQ(image.size, (Size{viewSize, viewSize}));
        ASSERT_GT(redAround(image, elevated), 10) << "the draped line is not drawn at its 1000 m projection";
        ASSERT_EQ(redAround(image, seaLevel), 0) << "the line is drawn at its sea-level projection";
        // The two points are far apart compared to the 6 px line, so the miss case is meaningful.
        ASSERT_GT(distance(), 40.0);
        for (const auto& p : {elevated, seaLevel}) {
            ASSERT_GT(p.x, 0);
            ASSERT_GT(p.y, 0);
            ASSERT_LT(p.x, viewSize);
            ASSERT_LT(p.y, viewSize);
        }
    }

    size_t query(const ScreenCoordinate& point) {
        RenderedQueryOptions options;
        options.layerIDs = std::vector<std::string>{"trail"};
        return frontend.getRenderer()->queryRenderedFeatures(point, options).size();
    }
};

} // namespace

TEST(TerrainQuery, DrapedLineAtPitch) {
    TerrainQueryScene scene;
    scene.expectRenderedScene();
    EXPECT_EQ(scene.query(scene.elevated), 1u) << "a tap on the draped line misses it";
}

TEST(TerrainQuery, SeaLevelPointMisses) {
    TerrainQueryScene scene;
    scene.expectRenderedScene();
    EXPECT_EQ(scene.query(scene.seaLevel), 0u)
        << "a tap where the line would sit at sea level (nothing is drawn there) hits it";
}
