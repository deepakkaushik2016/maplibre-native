#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/map/transform_state.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/style/image.hpp>
#include <mln/style/style.hpp>
#include <mln/util/image.hpp>
#include <mln/util/projection.hpp>
#include <mln/util/run_loop.hpp>

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace mln;

namespace {

// With terrain on, a symbol is drawn at its anchor lifted onto the terrain, so a tap on the icon
// must find it there, and not at the anchor's sea-level projection, where nothing is drawn.
//
// The scene is a flat 1000 m plateau, so where the icon is drawn is known exactly: its anchor
// projected at 1000 m times the exaggeration. Every case first checks from the rendered pixels that
// the icon really is drawn at the point it then queries.
constexpr double plateauMeters = 1000.0;
constexpr uint32_t viewSize = 512;
constexpr uint32_t iconSize = 16;
const LatLng center{46.0, 10.0};
// The POI, north of the centre so it is not on the screen's centre row.
const LatLng poi{46.01, 10.0};

std::string symbolStyle(std::optional<double> exaggeration) {
    const std::string terrain = exaggeration ? R"("terrain": {"source": "dem", "exaggeration": )" +
                                                   std::to_string(*exaggeration) + "},"
                                             : "";
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
    "poi": {
      "type": "geojson",
      "data": {
        "type": "Feature",
        "properties": {"name": "trailhead"},
        "geometry": {"type": "Point", "coordinates": [10.0, 46.01]}
      }
    }
  },
  )STYLE" + terrain +
           R"STYLE(
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "white"}},
    {"id": "poi", "type": "symbol", "source": "poi",
     "layout": {"icon-image": "marker", "icon-allow-overlap": true}}
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

PremultipliedImage redSquare() {
    PremultipliedImage image({iconSize, iconSize});
    for (uint32_t i = 0; i < iconSize * iconSize; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        px[0] = 255;
        px[1] = 0;
        px[2] = 0;
        px[3] = 255;
    }
    return image;
}

// Map::pixelForLatLng (screen pixels, y down - what queryRenderedFeatures takes), but for a point
// `meters` above sea level instead of on it.
ScreenCoordinate project(const TransformState& state, const LatLng& latLng, double meters) {
    const Point<double> pt = Projection::project(latLng, state.getScale());
    const vec4 c = {{pt.x, pt.y, meters, 1}};
    vec4 p;
    matrix::transformMat4(p, c, state.getProjectionMatrix());
    const Size size = state.getSize();
    const double x = (p[0] / p[3] + 1) * size.width / 2;
    const double yUp = (p[1] / p[3] + 1) * size.height / 2;
    return {x, size.height - yUp};
}

bool isRed(const PremultipliedImage& image, int x, int y) {
    const uint8_t* px = image.data.get() + (static_cast<size_t>(y) * image.size.width + x) * 4;
    return px[0] > 150 && px[1] < 100 && px[2] < 100;
}

// Red pixels within `radius` of p: the 16 px icon centred on p fills all 49 of a radius of 3.
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

struct TerrainSymbolScene {
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
    // Where the icon is drawn, and where its anchor sits at sea level.
    ScreenCoordinate drawn;
    ScreenCoordinate seaLevel;

    /// `exaggeration` empty: no terrain in the style.
    explicit TerrainSymbolScene(std::optional<double> exaggeration) {
        fileSource->tileResponse = [this](const Resource&) {
            Response res;
            res.data = std::make_shared<std::string>(tile);
            return res;
        };
        map.getStyle().loadJSON(symbolStyle(exaggeration));
        map.getStyle().addImage(std::make_unique<style::Image>("marker", redSquare(), 1.0f));
        const double surface = plateauMeters * exaggeration.value_or(0.0);
        map.jumpTo(CameraOptions().withCenter(center).withZoom(13.0).withPitch(60.0).withCenterAltitude(surface));
        for (int i = 0; i < 4; ++i) {
            loop.runOnce();
            image = frontend.render(map).image;
        }
        const auto state = frontend.getTransformState();
        EXPECT_TRUE(state);
        if (state) {
            drawn = project(*state, poi, surface);
            seaLevel = project(*state, poi, 0.0);
        }
    }

    double distance() const { return std::hypot(drawn.x - seaLevel.x, drawn.y - seaLevel.y); }

    ::testing::Message where() const {
        return ::testing::Message() << "drawn (" << drawn.x << ", " << drawn.y << "), sea level (" << seaLevel.x << ", "
                                    << seaLevel.y << "), " << distance() << " px apart";
    }

    // The frame really shows the icon where the elevated projection says, and nothing where the
    // sea-level projection says; without this a query result says nothing.
    void expectIconDrawnAboveSeaLevel() const {
        SCOPED_TRACE(where());
        ASSERT_EQ(image.size, (Size{viewSize, viewSize}));
        for (const auto& p : {drawn, seaLevel}) {
            ASSERT_GT(p.x, 0);
            ASSERT_GT(p.y, 0);
            ASSERT_LT(p.x, viewSize);
            ASSERT_LT(p.y, viewSize);
        }
        ASSERT_EQ(redAround(drawn), 49) << "the icon is not drawn at its elevated projection";
        ASSERT_EQ(redAround(seaLevel, 10), 0) << "the icon is drawn near its sea-level projection";
        // Far apart compared to the 16 px icon, so the miss case is meaningful.
        ASSERT_GT(distance(), 40.0);
    }

    int redAround(const ScreenCoordinate& p, int radius = 3) const { return ::redAround(image, p, radius); }

    size_t query(const ScreenCoordinate& point) {
        RenderedQueryOptions options;
        options.layerIDs = std::vector<std::string>{"poi"};
        return frontend.getRenderer()->queryRenderedFeatures(point, options).size();
    }

    // A box query of the `radius` px square around `point`.
    size_t queryBox(const ScreenCoordinate& point, double radius) {
        RenderedQueryOptions options;
        options.layerIDs = std::vector<std::string>{"poi"};
        const ScreenBox box{{point.x - radius, point.y - radius}, {point.x + radius, point.y + radius}};
        return frontend.getRenderer()->queryRenderedFeatures(box, options).size();
    }
};

} // namespace

// A trailhead icon on a 1000 m plateau, exaggerated 1.25x, at pitch 60.
TEST(TerrainSymbolQuery, TapOnElevatedIconHitsIt) {
    TerrainSymbolScene scene(1.25);
    scene.expectIconDrawnAboveSeaLevel();
    SCOPED_TRACE(scene.where());
    EXPECT_EQ(scene.query(scene.drawn), 1u) << "a tap on the icon, where it is drawn, misses it";
    EXPECT_EQ(scene.query(scene.seaLevel), 0u)
        << "a tap where the icon would sit at sea level (nothing is drawn there) hits it";
}

TEST(TerrainSymbolQuery, BoxAroundElevatedIconHitsIt) {
    TerrainSymbolScene scene(1.25);
    scene.expectIconDrawnAboveSeaLevel();
    SCOPED_TRACE(scene.where());
    EXPECT_EQ(scene.queryBox(scene.drawn, 20.0), 1u) << "a box around the drawn icon misses it";
    EXPECT_EQ(scene.queryBox(scene.seaLevel, 20.0), 0u)
        << "a box around where the icon would sit at sea level (nothing is drawn there) hits it";
}

// At exaggeration 0 the terrain is flat at sea level, so the icon is where the 2D map puts it.
TEST(TerrainSymbolQuery, ExaggerationZeroQueriesAtSeaLevel) {
    TerrainSymbolScene scene(0.0);
    ASSERT_EQ(scene.redAround(scene.seaLevel), 49) << "the icon is not drawn at its sea-level projection";
    EXPECT_EQ(scene.query(scene.seaLevel), 1u) << "a tap on the icon misses it";
    EXPECT_EQ(scene.queryBox(scene.seaLevel, 20.0), 1u) << "a box around the icon misses it";
}

TEST(TerrainSymbolQuery, TerrainOffQueriesAtSeaLevel) {
    TerrainSymbolScene scene(std::nullopt);
    ASSERT_EQ(scene.redAround(scene.seaLevel), 49) << "the icon is not drawn at its sea-level projection";
    EXPECT_EQ(scene.query(scene.seaLevel), 1u) << "a tap on the icon misses it";
    EXPECT_EQ(scene.queryBox(scene.seaLevel, 20.0), 1u) << "a box around the icon misses it";
}
