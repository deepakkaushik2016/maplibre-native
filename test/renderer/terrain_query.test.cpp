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

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

// Red pixels within `radius` of p (the line is 6 px wide, so a hit has several), or within
// `radiusX` across for a window wider than it is tall.
int redAround(const PremultipliedImage& image, const ScreenCoordinate& p, int radius = 3, int radiusX = 0) {
    radiusX = std::max(radius, radiusX);
    int count = 0;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radiusX; dx <= radiusX; ++dx) {
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

namespace {

// A DEM that rises linearly northwards in web-mercator y. Pixel i of a tile holds the height at
// its centre, (i + 0.5) / 256 across it, where the samplers (the shaders' get_elevation,
// RenderTerrain's sampleDEM) place it, as maplibre-gl-js does, so bilinear interpolation reproduces
// the plane exactly: the drawn surface and the
// height under a point are known from the DEM tile that covers it. The z14 tiles hold the plane;
// coarser tiles hold it raised by rampAncestorMeters, so it shows which tile a sample came from.
constexpr double rampBaseMeters = 600.0;
constexpr double rampMetersPerWorld = 5.0e6;
constexpr double rampAncestorMeters = 60.0;
const double rampOriginY = Projection::project(center, 1.0).y / util::tileSize_D;

double rampMeters(double worldY) {
    return rampBaseMeters + (rampOriginY - worldY) * rampMetersPerWorld;
}

double rampMeters(const LatLng& latLng) {
    return rampMeters(Projection::project(latLng, 1.0).y / util::tileSize_D);
}

std::string rampTile(const CanonicalTileID& id) {
    PremultipliedImage image({256, 256});
    const double tiles = std::pow(2.0, id.z);
    for (uint32_t y = 0; y < 256; ++y) {
        const double meters = rampMeters((id.y + (y + 0.5) / 256.0) / tiles) + (id.z < 14 ? rampAncestorMeters : 0.0);
        const auto value = static_cast<uint32_t>(std::lround((meters + 10000.0) * 10.0));
        for (uint32_t x = 0; x < 256; ++x) {
            uint8_t* px = image.data.get() + (static_cast<size_t>(y) * 256 + x) * 4;
            px[0] = static_cast<uint8_t>(value >> 16);
            px[1] = static_cast<uint8_t>((value >> 8) & 0xff);
            px[2] = static_cast<uint8_t>(value & 0xff);
            px[3] = 255;
        }
    }
    return encodePNG(image);
}

// East-west trails at these latitudes, near to far from the camera.
constexpr std::array<double, 4> rampTrailLats{{45.996, 46.006, 46.016, 46.03}};

std::string rampStyle() {
    std::string features;
    for (size_t i = 0; i < rampTrailLats.size(); ++i) {
        const std::string lat = std::to_string(rampTrailLats[i]);
        features += std::string(i ? "," : "") + R"({"type": "Feature", "properties": {"name": ")" + std::to_string(i) +
                    R"("}, "geometry": {"type": "LineString", "coordinates": [[9.97, )" + lat + "], [10.03, " + lat +
                    "]]}}";
    }
    return R"STYLE({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"],
      "encoding": "mapbox",
      "maxzoom": 14,
      "tileSize": 256
    },
    "trail": {"type": "geojson", "data": {"type": "FeatureCollection", "features": [)STYLE" +
           features + R"STYLE(]}}
  },
  "terrain": {"source": "dem", "exaggeration": 1.5},
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "white"}},
    {"id": "trail", "type": "line", "source": "trail", "paint": {"line-color": "#d00", "line-width": 6}}
  ]
})STYLE";
}

} // namespace

// A tap's raycast samples the DEM along the ray, through several DEM tiles and, where a z14 tile
// is not loaded (under the camera, and the one that never arrives under the third trail), through
// their z10 ancestor (prefetched, it stands in for them) 60 m higher. Each trail is drawn at the
// height of the tile under it, so each tap must find exactly the trail it is drawn on, nearest to
// farthest: on z14 tiles behind the ancestor, on the ancestor, and behind both.
TEST(TerrainQuery, RaycastAcrossDEMTilesAndAncestorsHitsWhereDrawn) {
    constexpr double exaggeration = 1.5;
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    // The z14 tile under the third trail never answers.
    const auto underThird = Projection::project({rampTrailLats[2], center.longitude()}, 1.0);
    const CanonicalTileID missing{14,
                                  static_cast<uint32_t>(underThird.x / util::tileSize_D * 16384),
                                  static_cast<uint32_t>(underThird.y / util::tileSize_D * 16384)};
    bool ancestorServed = false;
    fileSource->tileResponse = [&](const Resource& resource) -> std::optional<Response> {
        const auto& t = *resource.tileData;
        const CanonicalTileID id{static_cast<uint8_t>(t.z), static_cast<uint32_t>(t.x), static_cast<uint32_t>(t.y)};
        if (id == missing) {
            return std::nullopt;
        }
        ancestorServed = ancestorServed || missing.isChildOf(id);
        Response res;
        res.data = std::make_shared<std::string>(rampTile(id));
        return res;
    };
    HeadlessFrontend frontend{{viewSize, viewSize}, 1};
    MapAdapter map{frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Continuous).withSize(frontend.getSize())};
    map.getStyle().loadJSON(rampStyle());
    map.jumpTo(CameraOptions().withCenter(center).withZoom(13.0).withPitch(65.0).withCenterAltitude(rampMeters(center) *
                                                                                                    exaggeration));
    const auto pump = [&] {
        loop.runOnce();
        frontend.renderFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    // Loaded once each trail's own DEM tile is (or, for the third, its ancestor), up to 20 s; then
    // a few more frames so every drawable has been built over them.
    const auto loaded = [&] {
        for (size_t i = 0; i < rampTrailLats.size(); ++i) {
            const LatLng onTrail{rampTrailLats[i], center.longitude()};
            const auto elevation = frontend.getRenderer()->queryTerrainElevation(onTrail);
            const double meters = rampMeters(onTrail) + (i == 2 ? rampAncestorMeters : 0.0);
            if (!elevation || std::abs(*elevation - meters * exaggeration) > exaggeration) {
                return false;
            }
        }
        return ancestorServed;
    };
    for (int i = 0; i < 20000 && !loaded(); ++i) {
        pump();
    }
    for (int i = 0; i < 20; ++i) {
        pump();
    }
    const PremultipliedImage image = frontend.readStillImage();
    const auto state = frontend.getTransformState();
    ASSERT_TRUE(state);
    ASSERT_TRUE(ancestorServed) << "no ancestor of the missing DEM tile was loaded";
    const auto onAncestor = frontend.getRenderer()->queryTerrainElevation({rampTrailLats[2], center.longitude()});
    ASSERT_TRUE(onAncestor) << "nothing stands in for the missing DEM tile";
    // DEMData decodes to whole metres, so within 1 m (exaggerated).
    EXPECT_NEAR(*onAncestor,
                (rampMeters({rampTrailLats[2], center.longitude()}) + rampAncestorMeters) * exaggeration,
                exaggeration);

    RenderedQueryOptions options;
    options.layerIDs = std::vector<std::string>{"trail"};
    for (size_t i = 0; i < rampTrailLats.size(); ++i) {
        const LatLng onTrail{rampTrailLats[i], center.longitude()};
        const double meters = rampMeters(onTrail) + (i == 2 ? rampAncestorMeters : 0.0);
        const auto drawn = project(*state, onTrail, meters * exaggeration);
        SCOPED_TRACE(::testing::Message()
                     << "trail " << i << " at " << meters << " m, drawn at (" << drawn.x << ", " << drawn.y << ")");
        ASSERT_GT(drawn.y, 0);
        ASSERT_LT(drawn.y, viewSize);
        // The rows within a pixel of the trail's height, along the east-west trail (21 by 3 px):
        // the farthest is foreshortened to a single row. More than a fifth of the window red, as
        // the 7 px square this replaced asked: one full row of the trail at its height, while a
        // row a pixel or more off is outside the window.
        ASSERT_GT(redAround(image, drawn, 1, 10), 13) << "the trail is not drawn at its height on the ramp";
        const auto hits = frontend.getRenderer()->queryRenderedFeatures(drawn, options);
        ASSERT_EQ(hits.size(), 1u) << "a tap on the drawn trail does not find exactly one trail";
        EXPECT_EQ(hits[0].properties.at("name"), mapbox::feature::value(std::to_string(i)))
            << "a tap on the drawn trail finds another trail";
    }
}
