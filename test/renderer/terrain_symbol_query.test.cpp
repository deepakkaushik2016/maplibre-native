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
#include <mln/util/io.hpp>
#include <mln/util/projection.hpp>
#include <mln/util/run_loop.hpp>

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace mln;

namespace {

// With terrain on, a symbol is drawn at its anchor lifted onto the terrain, so a tap on it must
// find it there, and not at the anchor's sea-level projection, where nothing is drawn.
//
// Where the symbol is drawn is known exactly from the DEM: its anchor projected at the terrain
// height the shaders sample under it, times the exaggeration. Every case first checks from the
// rendered pixels that the symbol really is drawn at the point it then queries.
constexpr double plateauMeters = 1000.0;
constexpr double cliffTopMeters = 1600.0;
constexpr uint32_t viewSize = 512;
constexpr uint32_t iconSize = 16;
// The DEM is served at z10, 256 px tiles.
constexpr int demZoom = 10;
constexpr double demDim = 256.0;

double lngOfDEMPixel(double globalPixelX) {
    return globalPixelX / (std::pow(2.0, demZoom) * demDim) * 360.0 - 180.0;
}

// The cliff DEM: z10 tiles up to x = 540 are 1000 m, tiles from x = 541 on are 1600 m. The step is
// at the tile edge, so the last texel column of tile 540 rises to the first of tile 541 only in
// the DEM border tile 540 is backfilled with, which is what the shaders interpolate into.
constexpr int32_t cliffTileX = 541;
// A POI three quarters of the way across tile 540's last texel: drawn at 1000 + 0.75 * 600 m.
const double cliffPoiLng = lngOfDEMPixel(cliffTileX * demDim - 0.25);
constexpr double cliffPoiMeters = plateauMeters + 0.75 * (cliffTopMeters - plateauMeters);

enum class Dem {
    Plateau,
    Cliff
};
enum class Symbol {
    Icon,
    Text
};

struct SceneOptions {
    /// Empty: no terrain in the style.
    std::optional<double> exaggeration;
    Dem dem = Dem::Plateau;
    Symbol symbol = Symbol::Icon;
    double bearing = 0.0;

    double poiLng() const { return dem == Dem::Cliff ? cliffPoiLng : 10.0; }
    /// The terrain height the shaders lift the anchor by, before exaggeration.
    double poiMeters() const { return dem == Dem::Cliff ? cliffPoiMeters : plateauMeters; }
};

std::string symbolStyle(const SceneOptions& options) {
    const std::string terrain = options.exaggeration ? R"("terrain": {"source": "dem", "exaggeration": )" +
                                                           std::to_string(*options.exaggeration) + "},"
                                                     : "";
    const std::string layout = options.symbol == Symbol::Icon
                                   ? R"("icon-image": "marker", "icon-allow-overlap": true)"
                                   : R"("text-field": "WWW", "text-font": ["Open Sans Regular"], "text-size": 24,
                                        "text-allow-overlap": true)";
    const std::string paint = options.symbol == Symbol::Icon ? "{}" : R"({"text-color": "#f00"})";
    return R"STYLE({
  "version": 8,
  "glyphs": "http://example.com/fonts/{fontstack}/{range}.pbf",
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
        "geometry": {"type": "Point", "coordinates": [)STYLE" +
           std::to_string(options.poiLng()) + R"STYLE(, 46.01]}
      }
    }
  },
  )STYLE" + terrain +
           R"STYLE(
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "white"}},
    {"id": "poi", "type": "symbol", "source": "poi", "layout": {)STYLE" +
           layout + R"STYLE(}, "paint": )STYLE" + paint + R"STYLE(}
  ]
})STYLE";
}

// A Terrain-RGB tile all at `meters`: -10000 + (R * 65536 + G * 256 + B) * 0.1.
std::string demTile(double meters) {
    const auto value = static_cast<uint32_t>(std::lround((meters + 10000.0) * 10.0));
    PremultipliedImage image({256, 256});
    for (uint32_t i = 0; i < 256 * 256; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        px[0] = static_cast<uint8_t>(value >> 16);
        px[1] = static_cast<uint8_t>((value >> 8) & 0xff);
        px[2] = static_cast<uint8_t>(value & 0xff);
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
    std::string lowTile = demTile(plateauMeters);
    std::string highTile = demTile(cliffTopMeters);
    HeadlessFrontend frontend{{viewSize, viewSize}, 1};
    MapAdapter map{frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())};
    SceneOptions options;
    PremultipliedImage image;
    // Where the symbol is drawn, and where its anchor sits at sea level.
    ScreenCoordinate drawn;
    ScreenCoordinate seaLevel;
    // Where it would be drawn if lifted by the plateau's height instead of what the shaders sample.
    ScreenCoordinate onPlateau;

    explicit TerrainSymbolScene(SceneOptions options_)
        : options(options_) {
        fileSource->tileResponse = [this](const Resource& resource) {
            Response res;
            const bool high = options.dem == Dem::Cliff && resource.tileData && resource.tileData->x >= cliffTileX;
            res.data = std::make_shared<std::string>(high ? highTile : lowTile);
            return res;
        };
        fileSource->glyphsResponse = [](const Resource&) {
            Response res;
            res.data = std::make_shared<std::string>(util::read_file("test/fixtures/resources/glyphs.pbf"));
            return res;
        };
        map.getStyle().loadJSON(symbolStyle(options));
        map.getStyle().addImage(std::make_unique<style::Image>("marker", redSquare(), 1.0f));
        const double exaggeration = options.exaggeration.value_or(0.0);
        const LatLng poi{46.01, options.poiLng()};
        map.jumpTo(CameraOptions()
                       .withCenter(LatLng{46.0, options.poiLng()})
                       .withZoom(13.0)
                       .withPitch(60.0)
                       .withBearing(options.bearing)
                       .withCenterAltitude(plateauMeters * exaggeration));
        for (int i = 0; i < 4; ++i) {
            loop.runOnce();
            image = frontend.render(map).image;
        }
        const auto state = frontend.getTransformState();
        EXPECT_TRUE(state);
        if (state) {
            drawn = project(*state, poi, options.poiMeters() * exaggeration);
            seaLevel = project(*state, poi, 0.0);
            onPlateau = project(*state, poi, plateauMeters * exaggeration);
        }
    }

    static double distance(const ScreenCoordinate& a, const ScreenCoordinate& b) {
        return std::hypot(a.x - b.x, a.y - b.y);
    }

    ::testing::Message where() const {
        return ::testing::Message() << "drawn (" << drawn.x << ", " << drawn.y << "), sea level (" << seaLevel.x << ", "
                                    << seaLevel.y << "), on the plateau (" << onPlateau.x << ", " << onPlateau.y << ")";
    }

    // The frame really shows the symbol where the elevated projection says, and nothing where the
    // sea-level projection (or `other`) says; without this a query result says nothing.
    void expectDrawnAt(const ScreenCoordinate& other) const {
        SCOPED_TRACE(where());
        ASSERT_EQ(image.size, (Size{viewSize, viewSize}));
        for (const auto& p : {drawn, seaLevel, other}) {
            ASSERT_GT(p.x, 0);
            ASSERT_GT(p.y, 0);
            ASSERT_LT(p.x, viewSize);
            ASSERT_LT(p.y, viewSize);
        }
        if (options.symbol == Symbol::Icon) {
            ASSERT_EQ(redAround(drawn), 49) << "the icon is not drawn at its elevated projection";
        } else {
            // Text is glyph strokes, not a solid square: count over its middle.
            ASSERT_GT(redAround(drawn, 8), 20) << "the text is not drawn at its elevated projection";
        }
        ASSERT_EQ(redAround(other, 10), 0) << "the symbol is drawn near (" << other.x << ", " << other.y << ")";
        // Far apart compared to the 16 px icon or the 24 px text, so the miss case is meaningful.
        ASSERT_GT(distance(drawn, other), 40.0);
    }

    void expectDrawnAboveSeaLevel() const { expectDrawnAt(seaLevel); }

    int redAround(const ScreenCoordinate& p, int radius = 3) const { return ::redAround(image, p, radius); }

    size_t query(const ScreenCoordinate& point) {
        RenderedQueryOptions queryOptions;
        queryOptions.layerIDs = std::vector<std::string>{"poi"};
        return frontend.getRenderer()->queryRenderedFeatures(point, queryOptions).size();
    }

    // A box query of the `radius` px square around `point`.
    size_t queryBox(const ScreenCoordinate& point, double radius) {
        RenderedQueryOptions queryOptions;
        queryOptions.layerIDs = std::vector<std::string>{"poi"};
        const ScreenBox box{{point.x - radius, point.y - radius}, {point.x + radius, point.y + radius}};
        return frontend.getRenderer()->queryRenderedFeatures(box, queryOptions).size();
    }
};

} // namespace

// A trailhead icon on a 1000 m plateau, exaggerated 1.25x, at pitch 60.
TEST(TerrainSymbolQuery, TapOnElevatedIconHitsIt) {
    TerrainSymbolScene scene({.exaggeration = 1.25});
    scene.expectDrawnAboveSeaLevel();
    SCOPED_TRACE(scene.where());
    EXPECT_EQ(scene.query(scene.drawn), 1u) << "a tap on the icon, where it is drawn, misses it";
    EXPECT_EQ(scene.query(scene.seaLevel), 0u)
        << "a tap where the icon would sit at sea level (nothing is drawn there) hits it";
}

TEST(TerrainSymbolQuery, BoxAroundElevatedIconHitsIt) {
    TerrainSymbolScene scene({.exaggeration = 1.25});
    scene.expectDrawnAboveSeaLevel();
    SCOPED_TRACE(scene.where());
    EXPECT_EQ(scene.queryBox(scene.drawn, 20.0), 1u) << "a box around the drawn icon misses it";
    EXPECT_EQ(scene.queryBox(scene.seaLevel, 20.0), 0u)
        << "a box around where the icon would sit at sea level (nothing is drawn there) hits it";
}

// The icon stands in the last texel column of its DEM tile, at the foot of a 600 m step up to the
// next tile. The shaders interpolate into the DEM border (backfilled from that next tile), so the
// icon is drawn 450 m above the plateau; edge-clamped sampling would put its box on the plateau.
TEST(TerrainSymbolQuery, IconAtDEMTileEdgeHitsWhereDrawn) {
    TerrainSymbolScene scene({.exaggeration = 1.25, .dem = Dem::Cliff});
    scene.expectDrawnAt(scene.onPlateau);
    scene.expectDrawnAboveSeaLevel();
    SCOPED_TRACE(scene.where());
    EXPECT_EQ(scene.query(scene.drawn), 1u) << "a tap on the icon, where it is drawn, misses it";
    EXPECT_EQ(scene.query(scene.onPlateau), 0u)
        << "a tap where the icon would sit on the plateau (nothing is drawn there) hits it";
}

// Text goes through its own collision feature and matrix; the map is rotated, over the DEM step.
TEST(TerrainSymbolQuery, TextOnSlopeWithBearingHitsWhereDrawn) {
    TerrainSymbolScene scene({.exaggeration = 1.25, .dem = Dem::Cliff, .symbol = Symbol::Text, .bearing = 30.0});
    scene.expectDrawnAboveSeaLevel();
    SCOPED_TRACE(scene.where());
    EXPECT_EQ(scene.query(scene.drawn), 1u) << "a tap on the text, where it is drawn, misses it";
    EXPECT_EQ(scene.query(scene.seaLevel), 0u)
        << "a tap where the text would sit at sea level (nothing is drawn there) hits it";
    EXPECT_EQ(scene.queryBox(scene.drawn, 20.0), 1u) << "a box around the drawn text misses it";
}

// At exaggeration 0 the terrain is flat at sea level, so the icon is where the 2D map puts it.
TEST(TerrainSymbolQuery, ExaggerationZeroQueriesAtSeaLevel) {
    TerrainSymbolScene scene({.exaggeration = 0.0});
    ASSERT_EQ(scene.redAround(scene.seaLevel), 49) << "the icon is not drawn at its sea-level projection";
    EXPECT_EQ(scene.query(scene.seaLevel), 1u) << "a tap on the icon misses it";
    EXPECT_EQ(scene.queryBox(scene.seaLevel, 20.0), 1u) << "a box around the icon misses it";
}

TEST(TerrainSymbolQuery, TerrainOffQueriesAtSeaLevel) {
    TerrainSymbolScene scene({});
    ASSERT_EQ(scene.redAround(scene.seaLevel), 49) << "the icon is not drawn at its sea-level projection";
    EXPECT_EQ(scene.query(scene.seaLevel), 1u) << "a tap on the icon misses it";
    EXPECT_EQ(scene.queryBox(scene.seaLevel, 20.0), 1u) << "a box around the icon misses it";
}
