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
#include <memory>
#include <string>
#include <utility>

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

std::string queryStyle(double exaggeration) {
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
    "trail": {
      "type": "geojson",
      "data": {
        "type": "Feature",
        "properties": {"name": "trail"},
        "geometry": {"type": "LineString", "coordinates": [[9.97, 46.01], [10.03, 46.01]]}
      }
    }
  },
  "terrain": {"source": "dem", "exaggeration": )STYLE" +
           std::to_string(exaggeration) + R"STYLE(},
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "white"}},
    {"id": "trail", "type": "line", "source": "trail",
     "paint": {"line-color": "#d00", "line-width": 6}}
  ]
})STYLE";
}

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

// A 1000 m plateau cut by a north-south valley floored at sea level, valleyHalfWidth degrees either
// side of the centre's longitude.
constexpr double valleyHalfWidth = 0.004;

std::string valleyTile(const CanonicalTileID& id) {
    // Terrain-RGB: RGB(1, 134, 160) is 0 m, RGB(1, 173, 176) 1000 m.
    PremultipliedImage image({256, 256});
    const double tiles = std::pow(2.0, id.z);
    for (uint32_t y = 0; y < 256; ++y) {
        for (uint32_t x = 0; x < 256; ++x) {
            const double lng = (id.x + (x + 0.5) / 256.0) / tiles * 360.0 - 180.0;
            const bool valley = std::abs(lng - center.longitude()) < valleyHalfWidth;
            uint8_t* px = image.data.get() + (static_cast<size_t>(y) * 256 + x) * 4;
            px[0] = 1;
            px[1] = valley ? 134 : 173;
            px[2] = valley ? 160 : 176;
            px[3] = 255;
        }
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
            if (x >= 0 && y >= 0 && std::cmp_less(x, image.size.width) && std::cmp_less(y, image.size.height)) {
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

    explicit TerrainQueryScene(double exaggeration = 1.0, bool valley = false) {
        fileSource->tileResponse = [this, valley](const Resource& resource) {
            Response res;
            if (valley && resource.tileData) {
                const auto& t = *resource.tileData;
                res.data = std::make_shared<std::string>(
                    valleyTile({static_cast<uint8_t>(t.z), static_cast<uint32_t>(t.x), static_cast<uint32_t>(t.y)}));
            } else {
                res.data = std::make_shared<std::string>(tile);
            }
            return res;
        };
        map.getStyle().loadJSON(queryStyle(exaggeration));
        map.jumpTo(CameraOptions().withCenter(center).withZoom(13.0).withPitch(50.0).withCenterAltitude(plateauMeters));
        for (int i = 0; i < 4; ++i) {
            loop.runOnce();
            image = frontend.render(map).image;
        }
        const auto state = frontend.getTransformState();
        EXPECT_TRUE(state);
        if (state) {
            elevated = project(*state, onLine, plateauMeters * exaggeration);
            seaLevel = project(*state, onLine, 0.0);
        }
    }

    double distance() const { return std::hypot(elevated.x - seaLevel.x, elevated.y - seaLevel.y); }

    // The frame really shows the draped line where the elevated projection says, and nothing
    // where the sea-level projection says; without this a query result says nothing.
    void expectRenderedScene() const {
        SCOPED_TRACE(::testing::Message() << "elevated (" << elevated.x << ", " << elevated.y << "), sea level ("
                                          << seaLevel.x << ", " << seaLevel.y << "), " << distance() << " px apart");
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

    size_t queryBox(const ScreenBox& box) {
        RenderedQueryOptions options;
        options.layerIDs = std::vector<std::string>{"trail"};
        return frontend.getRenderer()->queryRenderedFeatures(box, options).size();
    }

    // A box query of the `radius` px square around `point`.
    size_t queryBox(const ScreenCoordinate& point, double radius) {
        return queryBox({{point.x - radius, point.y - radius}, {point.x + radius, point.y + radius}});
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

TEST(TerrainQuery, BoxAroundDrapedLine) {
    TerrainQueryScene scene;
    scene.expectRenderedScene();
    EXPECT_EQ(scene.queryBox(scene.elevated, 10.0), 1u) << "a box around the draped line misses it";
    EXPECT_EQ(scene.queryBox(scene.seaLevel, 10.0), 0u)
        << "a box around where the line would sit at sea level (nothing is drawn there) hits it";
}

// At exaggeration 0 the surface is drawn flat at sea level, so the line is where the 2D map puts it.
TEST(TerrainQuery, ExaggerationZeroQueriesAtSeaLevel) {
    TerrainQueryScene scene(0.0);
    ASSERT_GT(redAround(scene.image, scene.seaLevel), 10) << "the line is not drawn at its sea-level projection";
    EXPECT_EQ(scene.query(scene.seaLevel), 1u) << "a tap on the line misses it";
    EXPECT_EQ(scene.queryBox(scene.seaLevel, 10.0), 1u) << "a box around the line misses it";
}

// A box's edges run straight on screen, but over relief they do not run straight across the ground
// the sources match against: the terrain under an edge's middle can sit far lower than under its
// corners. Here the box's top edge crosses a valley (0 m) between corners on the plateau (1000 m),
// with the line on the valley floor just inside the edge. Resolving only the corners on the terrain
// moves the whole edge as far as the plateau's relief (~50 px) and drops the line.
TEST(TerrainQuery, BoxEdgeOverReliefKeepsFeatureJustInside) {
    TerrainQueryScene scene(1.0, true);
    const auto state = scene.frontend.getTransformState();
    ASSERT_TRUE(state);
    // The line crosses the valley floor, so it is drawn at its sea-level projection.
    const ScreenCoordinate line = scene.seaLevel;
    ASSERT_GT(redAround(scene.image, line), 10) << "the line is not drawn on the valley floor";
    const auto floor = scene.frontend.getRenderer()->queryTerrainElevation(onLine);
    ASSERT_TRUE(floor);
    ASSERT_NEAR(*floor, 0.0, 1.0);

    // The box's top corners sit on the plateau, well clear of the valley: the plateau's edges are
    // drawn inside the box.
    const double halfWidth = 150.0;
    const ScreenBox inside{{line.x - halfWidth, line.y - 12.0}, {line.x + halfWidth, line.y + 100.0}};
    for (const double side : {-1.0, 1.0}) {
        const auto plateauEdge = project(
            *state, {onLine.latitude(), center.longitude() + side * 2 * valleyHalfWidth}, plateauMeters);
        ASSERT_GT(plateauEdge.x, inside.min.x) << "the box's top corners are not clear of the valley";
        ASSERT_LT(plateauEdge.x, inside.max.x) << "the box's top corners are not clear of the valley";
    }

    EXPECT_EQ(scene.queryBox(inside), 1u) << "a box whose top edge runs 12 px above the line misses it";
    const ScreenBox below{{line.x - halfWidth, line.y + 12.0}, {line.x + halfWidth, line.y + 100.0}};
    EXPECT_EQ(scene.queryBox(below), 0u) << "a box whose top edge runs 12 px below the line hits it";
}
