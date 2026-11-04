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
#include <mln/util/constants.hpp>
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

// DEM pixel i covers [i, i + 1) and describes the terrain at its centre, i + 0.5, as in
// maplibre-gl-js; between two pixel centres the terrain is linear.
//
// The cliff DEM: z10 tiles up to x = 540 are 1000 m, tiles from x = 541 on are 1600 m. The step is
// at the tile edge, so the first pixel column of tile 541 falls to the last of tile 540 only in
// the DEM border tile 541 is backfilled with, which is what the shaders interpolate into.
constexpr int32_t cliffTileX = 541;
// A POI a quarter of the way into tile 541's first pixel, before that pixel's centre: three
// quarters of the way from the centre of tile 540's last pixel to it, so drawn at
// 1000 + 0.75 * 600 m.
const double cliffPoiLng = lngOfDEMPixel(cliffTileX * demDim + 0.25);
constexpr double cliffPoiMeters = plateauMeters + 0.75 * (cliffTopMeters - plateauMeters);

// The ridge DEM: one column of pixels at 1600 m, in the tile holding the POI at lng 10 (z10
// x = 540, pixel column 113), and 1000 m everywhere else. A POI at the centre of the ridge's
// pixels is drawn at the ridge's 1600 m; with the pixels at their corners it would be drawn
// halfway down its flank, at 1300 m.
constexpr int32_t ridgeTileX = 540;
constexpr uint32_t ridgeColumn = 113;
const double ridgePoiLng = lngOfDEMPixel(ridgeTileX * demDim + ridgeColumn + 0.5);

enum class Dem {
    Plateau,
    Cliff,
    Ridge,
    // A DEM served to z14, where the tiles with an odd x stand at `cliffTopMeters` and every
    // other tile (any other zoom included) at `plateauMeters`. The POI at lng 10 is in z14 tile
    // x = 8647, so it is drawn at the cliff top only if its DEM is that z14 tile.
    Z14Stripes,
    // A slope served at the zoom where `demTileSize` px tiles match the map: two zooms finer than
    // the 512 px GeoJSON tiles for 128 px tiles, three for 64 px ones, so the z13 symbol tile is
    // lifted by a DEM stitched from descendants two or three zooms deeper. Every coarser tile
    // holds the slope `slopeAncestorMeters` higher, so a symbol lifted by one of them shows.
    Slope,
    // Flat at fractionalMeters(): less than a metre, all of it fraction.
    Fractional
};

// A height that is all fraction, at a step of each encoding: 255/256 m in Terrarium, 0.9 m in
// Mapbox. Rounded to whole metres it is sea level.
double fractionalMeters(bool terrarium) {
    return terrarium ? 255.0 / 256.0 : 0.9;
}
enum class Symbol {
    Icon,
    Text,
    Circle
};

constexpr int stripesZoom = 14;
// The first z14 tile at the cliff top under the POI's z13 tile (x = 4323, children 8646 and 8647).
constexpr int64_t stripeTileX = 8647;

struct SceneOptions {
    /// Empty: no terrain in the style.
    std::optional<double> exaggeration;
    Dem dem = Dem::Plateau;
    Symbol symbol = Symbol::Icon;
    double bearing = 0.0;
    /// The DEM source's tile size: at 256 px its tiles are one zoom finer than the 512 px
    /// GeoJSON tiles at the same map zoom.
    uint32_t demTileSize = 256;
    /// The DEM's encoding: Terrarium, or else Mapbox Terrain-RGB.
    bool terrarium = false;
    /// Z14Stripes only: the POI a quarter of the way into the first pixel column of the cliff-top
    /// child (z14 x = 8647), on the step up from the plateau child before it: three quarters of
    /// the way from the centre of the plateau child's last pixel to that pixel's centre.
    bool atStripeSeam = false;

    double poiLng() const {
        if (dem == Dem::Z14Stripes && atStripeSeam) {
            return (static_cast<double>(stripeTileX) * 256.0 + 0.25) / (std::pow(2.0, stripesZoom) * 256.0) * 360.0 -
                   180.0;
        }
        if (dem == Dem::Ridge) {
            return ridgePoiLng;
        }
        return dem == Dem::Cliff ? cliffPoiLng : 10.0;
    }
    /// The terrain height the shaders lift the anchor by, before exaggeration.
    double poiMeters() const {
        switch (dem) {
            case Dem::Cliff:
                return cliffPoiMeters;
            case Dem::Ridge:
                return cliffTopMeters;
            case Dem::Slope:
                return plateauMeters;
            case Dem::Fractional:
                return fractionalMeters(terrarium);
            case Dem::Z14Stripes:
                // At z13 a 512 px DEM is served at z13, all plateau; a 256 px one at z14.
                if (demTileSize != 256) {
                    return plateauMeters;
                }
                return atStripeSeam ? plateauMeters + 0.75 * (cliffTopMeters - plateauMeters) : cliffTopMeters;
            case Dem::Plateau:
                break;
        }
        return plateauMeters;
    }
    int demMaxZoom() const {
        if (dem == Dem::Slope) {
            return 13 + static_cast<int>(std::lround(std::log2(512.0 / demTileSize)));
        }
        return dem == Dem::Z14Stripes ? stripesZoom : demZoom;
    }
};

std::string symbolStyle(const SceneOptions& options) {
    const std::string terrain = options.exaggeration ? R"("terrain": {"source": "dem", "exaggeration": )" +
                                                           std::to_string(*options.exaggeration) + "},"
                                                     : "";
    std::string type = "symbol";
    std::string layout;
    std::string paint = "{}";
    switch (options.symbol) {
        case Symbol::Icon:
            layout = R"("icon-image": "marker", "icon-allow-overlap": true)";
            break;
        case Symbol::Text:
            layout = R"("text-field": "WWW", "text-font": ["Open Sans Regular"], "text-size": 24,
                        "text-allow-overlap": true)";
            paint = R"({"text-color": "#f00"})";
            break;
        case Symbol::Circle:
            type = "circle";
            paint = R"({"circle-color": "#f00", "circle-radius": 8, "circle-pitch-alignment": "viewport"})";
            break;
    }
    return R"STYLE({
  "version": 8,
  "glyphs": "http://example.com/fonts/{fontstack}/{range}.pbf",
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"],
      "encoding": ")STYLE" +
           std::string(options.terrarium ? "terrarium" : "mapbox") + R"STYLE(",
      "maxzoom": )STYLE" +
           std::to_string(options.demMaxZoom()) + R"STYLE(,
      "tileSize": )STYLE" +
           std::to_string(options.demTileSize) + R"STYLE(
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
    {"id": "poi", "type": ")STYLE" +
           type + R"STYLE(", "source": "poi", "layout": {)STYLE" + layout + R"STYLE(}, "paint": )STYLE" + paint +
           R"STYLE(}
  ]
})STYLE";
}

// A DEM tile all at `meters`, but for the pixel column `ridge`, if any, which is at `ridgeMeters`:
// Terrarium (R * 256 + G + B / 256 - 32768) or Mapbox Terrain-RGB
// (-10000 + (R * 65536 + G * 256 + B) * 0.1), to the nearest step.
std::string demTile(bool terrarium,
                    double meters,
                    uint32_t size = 256,
                    std::optional<uint32_t> ridge = std::nullopt,
                    double ridgeMeters = 0.0) {
    const auto encode = [terrarium](double m) {
        return static_cast<uint32_t>(terrarium ? std::lround((m + 32768.0) * 256.0)
                                               : std::lround((m + 10000.0) * 10.0));
    };
    const uint32_t value = encode(meters);
    const uint32_t ridgeValue = encode(ridgeMeters);
    PremultipliedImage image({size, size});
    for (uint32_t i = 0; i < size * size; ++i) {
        const uint32_t v = ridge && i % size == *ridge ? ridgeValue : value;
        uint8_t* px = image.data.get() + i * 4;
        px[0] = static_cast<uint8_t>(v >> 16);
        px[1] = static_cast<uint8_t>((v >> 8) & 0xff);
        px[2] = static_cast<uint8_t>(v & 0xff);
        px[3] = 255;
    }
    return encodePNG(image);
}

constexpr double slopeAncestorMeters = 300.0;

// The slope DEM's tile `id` (`size` px, served up to `maxZoom`): in pixels of the `maxZoom` tiles,
// counted from the POI at lng 10, lat 46.01, the terrain is plateauMeters + 2 east + 3 north,
// and each pixel holds it at its centre.
std::string slopeTile(const CanonicalTileID& id, uint32_t size, int maxZoom) {
    const double scale = std::pow(2.0, maxZoom - id.z);
    const double pixels = std::pow(2.0, maxZoom) * size;
    const Point<double> poi = Projection::project(LatLng{46.01, 10.0}, 1.0);
    const double poiX = poi.x / util::tileSize_D * pixels;
    const double poiY = poi.y / util::tileSize_D * pixels;
    const double offset = std::cmp_less(id.z, maxZoom) ? slopeAncestorMeters : 0.0;
    PremultipliedImage image({size, size});
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const double u = (static_cast<double>(id.x) * size + x + 0.5) * scale - poiX;
            const double v = (static_cast<double>(id.y) * size + y + 0.5) * scale - poiY;
            const double meters = plateauMeters + 2.0 * u - 3.0 * v + offset;
            const auto value = static_cast<uint32_t>(std::lround((meters + 10000.0) * 10.0));
            uint8_t* px = image.data.get() + (static_cast<size_t>(y) * size + x) * 4;
            px[0] = static_cast<uint8_t>(value >> 16);
            px[1] = static_cast<uint8_t>((value >> 8) & 0xff);
            px[2] = static_cast<uint8_t>(value & 0xff);
            px[3] = 255;
        }
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
    HeadlessFrontend frontend{{viewSize, viewSize}, 1};
    MapAdapter map{frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())};
    SceneOptions options;
    std::string lowTile;
    std::string highTile;
    std::string ridgeTile;
    PremultipliedImage image;
    // Where the symbol is drawn, and where its anchor sits at sea level.
    ScreenCoordinate drawn;
    ScreenCoordinate seaLevel;
    // Where it would be drawn if lifted by the plateau's height instead of what the shaders sample.
    ScreenCoordinate onPlateau;

    explicit TerrainSymbolScene(SceneOptions options_)
        : options(options_),
          lowTile(demTile(options.terrarium,
                          options.dem == Dem::Fractional ? fractionalMeters(options.terrarium) : plateauMeters,
                          options.demTileSize)),
          highTile(demTile(options.terrarium, cliffTopMeters, options.demTileSize)),
          ridgeTile(demTile(options.terrarium, plateauMeters, options.demTileSize, ridgeColumn, cliffTopMeters)) {
        fileSource->tileResponse = [this](const Resource& resource) {
            Response res;
            const auto& tile = resource.tileData;
            if (tile && options.dem == Dem::Slope) {
                const CanonicalTileID id{
                    static_cast<uint8_t>(tile->z), static_cast<uint32_t>(tile->x), static_cast<uint32_t>(tile->y)};
                res.data = std::make_shared<std::string>(slopeTile(id, options.demTileSize, options.demMaxZoom()));
                return res;
            }
            if (tile && options.dem == Dem::Ridge && tile->z == demZoom && tile->x == ridgeTileX) {
                res.data = std::make_shared<std::string>(ridgeTile);
                return res;
            }
            const bool high = tile && ((options.dem == Dem::Cliff && tile->x >= cliffTileX) ||
                                       (options.dem == Dem::Z14Stripes && tile->z == stripesZoom && tile->x % 2 == 1));
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
                       .withCenterAltitude((options.dem == Dem::Fractional ? options.poiMeters() : plateauMeters) *
                                           exaggeration));
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
        if (options.symbol != Symbol::Text) {
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

// The icon stands in the first pixel column of its DEM tile, before that pixel's centre, on a 600 m
// step down to the previous tile. The shaders interpolate into the DEM border (backfilled from that
// previous tile), so the icon is drawn 450 m above the plateau; edge-clamped sampling would put
// its box on the cliff top.
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

// A 256 px DEM is one zoom finer than the 512 px GeoJSON tiles at the same map zoom: the z13
// symbol tile has no DEM tile of its own or above it, only its z14 children. The icon must still
// be drawn on the terrain, at the height of the child it stands in (1600 m, not the 1000 m of its
// neighbour or of any coarser DEM), and a tap there must find it.
TEST(TerrainSymbolQuery, IconOverFinerDEMHitsWhereDrawn) {
    TerrainSymbolScene scene({.exaggeration = 1.25, .dem = Dem::Z14Stripes});
    scene.expectDrawnAt(scene.onPlateau);
    scene.expectDrawnAboveSeaLevel();
    SCOPED_TRACE(scene.where());
    EXPECT_EQ(scene.query(scene.drawn), 1u) << "a tap on the icon, where it is drawn, misses it";
    EXPECT_EQ(scene.query(scene.seaLevel), 0u)
        << "a tap where the icon would sit at sea level (nothing is drawn there) hits it";
    EXPECT_EQ(scene.queryBox(scene.drawn, 20.0), 1u) << "a box around the drawn icon misses it";
}

// Across the seam between two children the stitched DEM runs from one child's last pixel into the
// other's first, as the shaders do on the terrain mesh through the child's backfilled border: the
// icon is drawn 450 m above the plateau, and its box must be there too.
TEST(TerrainSymbolQuery, IconAtFinerDEMSeamHitsWhereDrawn) {
    TerrainSymbolScene scene({.exaggeration = 1.25, .dem = Dem::Z14Stripes, .atStripeSeam = true});
    scene.expectDrawnAt(scene.onPlateau);
    scene.expectDrawnAboveSeaLevel();
    SCOPED_TRACE(scene.where());
    EXPECT_EQ(scene.query(scene.drawn), 1u) << "a tap on the icon, where it is drawn, misses it";
    EXPECT_EQ(scene.query(scene.onPlateau), 0u)
        << "a tap where the icon would sit on the plateau (nothing is drawn there) hits it";
}

// The POI stands on a ridge one DEM pixel wide, at the centre of the ridge's pixels: it is drawn
// on the ridge's crest (1600 m, where maplibre-gl-js draws it), not halfway down its flank, and a
// tap there finds it.
TEST(TerrainSymbolQuery, IconOnARidgeOnePixelWideIsDrawnOnItsCrest) {
    TerrainSymbolScene scene({.exaggeration = 1.25, .dem = Dem::Ridge});
    const auto state = scene.frontend.getTransformState();
    ASSERT_TRUE(state);
    const LatLng poi{46.01, scene.options.poiLng()};
    // Where it would be drawn with each DEM pixel at its corner instead: halfway down the flank.
    const ScreenCoordinate onFlank = project(
        *state, poi, 0.5 * (plateauMeters + cliffTopMeters) * *scene.options.exaggeration);
    scene.expectDrawnAboveSeaLevel();
    SCOPED_TRACE(scene.where());
    ASSERT_EQ(scene.redAround(onFlank), 0) << "the icon is drawn where pixels at their corners would put it, at ("
                                           << onFlank.x << ", " << onFlank.y << ")";
    EXPECT_EQ(scene.query(scene.drawn), 1u) << "a tap on the icon, where it is drawn, misses it";
    EXPECT_EQ(scene.query(onFlank), 0u) << "a tap on the ridge's flank (nothing is drawn there) hits the icon";
}

// 128 px DEM tiles are served two zooms finer than the 512 px GeoJSON tiles, 64 px ones three: the
// z13 symbol tile is lifted by a DEM resampled from descendants two or three zooms deeper. The icon
// must be drawn on the slope at its own height (not on a coarser tile, 300 m higher), and a tap
// there must find it: the CPU reads the same stitched DEM the shaders draw it with.
void expectDrawnOnSlopeOfDescendants(uint32_t demTileSize) {
    TerrainSymbolScene scene({.exaggeration = 1.25, .dem = Dem::Slope, .demTileSize = demTileSize});
    const auto state = scene.frontend.getTransformState();
    ASSERT_TRUE(state);
    const ScreenCoordinate onAncestor = project(*state,
                                                LatLng{46.01, scene.options.poiLng()},
                                                (plateauMeters + slopeAncestorMeters) * *scene.options.exaggeration);
    scene.expectDrawnAt(onAncestor);
    scene.expectDrawnAboveSeaLevel();
    SCOPED_TRACE(scene.where());
    EXPECT_EQ(scene.query(scene.drawn), 1u) << "a tap on the icon, where it is drawn, misses it";
    EXPECT_EQ(scene.query(onAncestor), 0u) << "a tap where a coarser DEM would lift the icon hits it";
}

TEST(TerrainSymbolQuery, IconOverDEMTwoZoomsFinerHitsWhereDrawn) {
    expectDrawnOnSlopeOfDescendants(128);
}

TEST(TerrainSymbolQuery, IconOverDEMThreeZoomsFinerHitsWhereDrawn) {
    expectDrawnOnSlopeOfDescendants(64);
}

// The ground is less than a metre above sea level, all of it fraction (0.9 m in Mapbox, 255/256 m in
// Terrarium), exaggerated 500 times so that a fraction dropped shows on screen. The icon is drawn
// at the full height, and a tap there must find it: the CPU decodes the heights as the shaders do.
void expectFractionDrawnAndHit(bool terrarium) {
    TerrainSymbolScene scene({.exaggeration = 500.0, .dem = Dem::Fractional, .terrarium = terrarium});
    scene.expectDrawnAboveSeaLevel();
    SCOPED_TRACE(scene.where());
    EXPECT_EQ(scene.query(scene.drawn), 1u) << "a tap on the icon, where it is drawn, misses it";
    EXPECT_EQ(scene.query(scene.seaLevel), 0u)
        << "a tap where the icon would sit with the fraction dropped (nothing is drawn there) hits it";
}

TEST(TerrainSymbolQuery, IconOnAFractionOfAMetreHitsWhereDrawnTerrarium) {
    expectFractionDrawnAndHit(true);
}

TEST(TerrainSymbolQuery, IconOnAFractionOfAMetreHitsWhereDrawnMapbox) {
    expectFractionDrawnAndHit(false);
}

// Circles bind the DEM the same way (as do fill extrusions and heatmaps).
TEST(TerrainSymbolQuery, CircleOverFinerDEMDrawnElevated) {
    TerrainSymbolScene scene({.exaggeration = 1.25, .dem = Dem::Z14Stripes, .symbol = Symbol::Circle});
    scene.expectDrawnAt(scene.onPlateau);
    scene.expectDrawnAboveSeaLevel();
}

// A 512 px DEM sits at the GeoJSON tiles' zoom and covers the symbol tile itself: unchanged.
TEST(TerrainSymbolQuery, IconOverSameZoomDEMHitsWhereDrawn) {
    TerrainSymbolScene scene({.exaggeration = 1.25, .dem = Dem::Z14Stripes, .demTileSize = 512});
    scene.expectDrawnAboveSeaLevel();
    SCOPED_TRACE(scene.where());
    EXPECT_EQ(scene.query(scene.drawn), 1u) << "a tap on the icon, where it is drawn, misses it";
    EXPECT_EQ(scene.query(scene.seaLevel), 0u)
        << "a tap where the icon would sit at sea level (nothing is drawn there) hits it";
}
