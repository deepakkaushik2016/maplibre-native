#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_observer.hpp>
#include <mln/map/map_options.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/storage/response.hpp>
#include <mln/style/style.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace mln;

// A DEM tile's 2-texel border holds its neighbours' edge pixels, backfilled when a tile or its
// neighbour loads, and both the elevation query and the terrain mesh read heights within half a
// DEM pixel of a seam from it. These tests load the DEM tiles round one corner in each order the
// map meets them - a neighbour after the tile, a tile reloaded while its neighbour is kept, every
// tile leaving the view and coming back - and check the seam after each against the ground:
//   - the elevation query (Renderer::queryTerrainElevation) reads the plane the DEM holds;
//   - the frame draws what a map that loaded every tile at once draws (the mesh's heights come
//     from the DEM textures, the hillshade's from the DEM border).
//
// The ground is a plane, so the DEM's bilinear surface is the plane itself, seams included, once
// every border holds its neighbour's pixels: 300 m per km eastward and 200 m per km northward,
// steep mountainside, so a border left a copy of the tile's own edge is metres off within half a
// DEM pixel of the seam.

namespace {

constexpr uint32_t viewWidth = 390;
constexpr uint32_t viewHeight = 844;
constexpr int demMaxZoom = 11;
constexpr uint32_t demSize = 256;
constexpr double kmPerDegree = 111.32;
constexpr double eastSlope = 300.0;  // m per km
constexpr double northSlope = 200.0; // m per km
// The plane runs this far from the corner, and is level beyond, so the heights stay in range.
constexpr double planeHalfKm = 30.0;
constexpr double exaggeration = 1.5;

// The corner the tests look at: the north-west corner of z11 tile (1081, 732), at about 46° N,
// 10° E. The four DEM tiles round it meet there.
constexpr uint32_t cornerX = 1081;
constexpr uint32_t cornerY = 732;

LatLng corner() {
    const double n = std::pow(2.0, demMaxZoom);
    const double longitude = cornerX / n * 360.0 - 180.0;
    const double latitude = util::rad2deg(std::atan(std::sinh(M_PI * (1.0 - 2.0 * cornerY / n))));
    return {latitude, longitude};
}

double planeMeters(const LatLng& point) {
    const LatLng origin = corner();
    const double northKm = (point.latitude() - origin.latitude()) * kmPerDegree;
    const double eastKm = (point.longitude() - origin.longitude()) * kmPerDegree *
                          std::cos(util::deg2rad(origin.latitude()));
    return 1000.0 + eastSlope * std::clamp(eastKm, -planeHalfKm, planeHalfKm) +
           northSlope * std::clamp(northKm, -planeHalfKm, planeHalfKm);
}

// The DEM tile z/x/y of the plane, `offset` metres up, each pixel holding the height at its
// centre (where the samplers place it), in Mapbox Terrain-RGB.
std::string planeTile(const CanonicalTileID& id, double offset) {
    const double n = std::pow(2.0, id.z);
    PremultipliedImage image({demSize, demSize});
    for (uint32_t row = 0; row < demSize; ++row) {
        const double mercatorY = (id.y + (row + 0.5) / demSize) / n;
        const double latitude = util::rad2deg(std::atan(std::sinh(M_PI * (1.0 - 2.0 * mercatorY))));
        for (uint32_t col = 0; col < demSize; ++col) {
            const double longitude = (id.x + (col + 0.5) / demSize) / n * 360.0 - 180.0;
            const auto value = static_cast<uint32_t>(
                std::lround((planeMeters({latitude, longitude}) + offset + 10000.0) * 10.0));
            uint8_t* px = image.data.get() + (static_cast<size_t>(row) * demSize + col) * 4;
            px[0] = static_cast<uint8_t>(value >> 16);
            px[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            px[2] = static_cast<uint8_t>(value & 0xFF);
            px[3] = 255;
        }
    }
    return encodePNG(image);
}

// Lines every 100 m across the view, draped on the terrain, so that the drawn mesh's height
// shows: a mesh drawn metres off moves the lines on the screen.
std::string gridGeoJSON() {
    const LatLng origin = corner();
    const double kmPerLng = kmPerDegree * std::cos(util::deg2rad(origin.latitude()));
    std::string lines;
    for (int i = -40; i <= 40; ++i) {
        const double km = i * 0.1;
        const double lat = origin.latitude() + km / kmPerDegree;
        const double lng = origin.longitude() + km / kmPerLng;
        const double south = origin.latitude() - 4.0 / kmPerDegree;
        const double north = origin.latitude() + 4.0 / kmPerDegree;
        const double west = origin.longitude() - 4.0 / kmPerLng;
        const double east = origin.longitude() + 4.0 / kmPerLng;
        if (!lines.empty()) {
            lines += ",";
        }
        lines += "[[" + std::to_string(west) + "," + std::to_string(lat) + "],[" + std::to_string(east) + "," +
                 std::to_string(lat) + "]],";
        lines += "[[" + std::to_string(lng) + "," + std::to_string(south) + "],[" + std::to_string(lng) + "," +
                 std::to_string(north) + "]]";
    }
    return R"({"type": "MultiLineString", "coordinates": [)" + lines + "]}";
}

std::string seamStyle() {
    return R"STYLE({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"],
      "encoding": "mapbox",
      "maxzoom": )STYLE" +
           std::to_string(demMaxZoom) + R"STYLE(,
      "tileSize": 256
    },
    "grid": {"type": "geojson", "data": )STYLE" +
           gridGeoJSON() + R"STYLE(}
  },
  "terrain": {"source": "dem", "exaggeration": )STYLE" +
           std::to_string(exaggeration) + R"STYLE(},
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "white"}},
    {"id": "hillshade", "type": "hillshade", "source": "dem"},
    {"id": "grid", "type": "line", "source": "grid", "paint": {"line-color": "black", "line-width": 2}}
  ]
})STYLE";
}

// How the file source answers one DEM tile: what the offline cache holds (DefaultFileSource
// answers a tile from the cache first, then from the network), and whether the network answer
// is held back for now.
struct TileRule {
    /// Metres the cached copy is off the plane; no cached copy when empty.
    std::optional<double> cachedOffset;
    bool holdNetwork = false;
};

// A file source that, like DefaultFileSource, answers each tile from its cache first and from
// the network after, so a tile loads (and parses) twice when the cache has it.
class CachingFileSource : public StubFileSource {
public:
    CachingFileSource()
        : StubFileSource(ResourceOptions::Default(), ClientOptions()) {}
    bool supportsCacheOnlyRequests() const override { return true; }
};

class FrameObserver : public MapObserver {
public:
    void onDidFinishRenderingFrame(const RenderFrameStatus& status) override {
        ++frames;
        idleFrame = !status.needsRepaint;
    }
    void onTileAction(TileOperation op, const OverscaledTileID& id, const std::string& sourceID) override {
        if (op == TileOperation::StartParse) {
            parsing.emplace(id, sourceID);
            idleFrame = false;
        } else if (op == TileOperation::EndParse || op == TileOperation::Cancelled || op == TileOperation::Error) {
            parsing.erase({id, sourceID});
            idleFrame = false;
        }
    }
    int frames = 0;
    /// Tiles being parsed.
    std::set<std::pair<OverscaledTileID, std::string>> parsing;
    /// Whether the last frame found nothing more to draw, and no tile has changed since.
    bool idleFrame = false;
};

// A phone-sized map, run continuously so tiles arrive across frames as they do in an app, over
// the corner at z14 (overzoomed: the DEM stops at z11), pitched so the heights show.
struct SeamScene {
    util::RunLoop loop;
    std::shared_ptr<CachingFileSource> fileSource = std::make_shared<CachingFileSource>();
    HeadlessFrontend frontend{{viewWidth, viewHeight}, 1};
    FrameObserver observer;
    MapAdapter map{
        frontend, observer, fileSource, MapOptions().withMapMode(MapMode::Continuous).withSize(frontend.getSize())};
    std::map<CanonicalTileID, TileRule> rules;
    /// Network answers per DEM tile.
    std::map<CanonicalTileID, int> networkLoads;

    explicit SeamScene(std::map<CanonicalTileID, TileRule> rules_ = {})
        : rules(std::move(rules_)) {
        fileSource->tileResponse = [this](const Resource& resource) -> std::optional<Response> {
            Response res;
            if (!resource.tileData || resource.url.find("/dem/") == std::string::npos) {
                res.noContent = true;
                return res;
            }
            const CanonicalTileID id(
                resource.tileData->z, static_cast<uint32_t>(resource.tileData->x), resource.tileData->y);
            const auto rule = rules.find(id);
            if (resource.loadingMethod == Resource::LoadingMethod::CacheOnly) {
                if (rule == rules.end() || !rule->second.cachedOffset) {
                    res.error = std::make_unique<Response::Error>(Response::Error::Reason::NotFound, "not cached");
                    return res;
                }
                res.data = std::make_shared<std::string>(planeTile(id, *rule->second.cachedOffset));
                return res;
            }
            if (rule != rules.end() && rule->second.holdNetwork) {
                return std::nullopt; // still on its way
            }
            ++networkLoads[id];
            res.data = std::make_shared<std::string>(planeTile(id, 0.0));
            return res;
        };
        map.getStyle().loadJSON(seamStyle());
        lookAt(corner());
    }

    void lookAt(const LatLng& center) {
        map.jumpTo(
            CameraOptions().withCenter(center).withZoom(14.0).withPitch(50.0).withBearing(0.0).withCenterAltitude(
                planeMeters(center) * exaggeration));
        settle();
    }

    // Runs the map until every tile that can arrive has, has been parsed and drawn, and a frame
    // has found nothing more to draw: at most 30 s, failing the test past that.
    void settle() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        // Quiet once a frame drawn since the call (idleFrame is cleared here and set only by a
        // frame) found nothing more to draw and no tile is being parsed. A slow software renderer
        // may draw only one or two frames for a change, so no more are required.
        const int start = observer.frames;
        observer.idleFrame = false;
        const auto quiet = [&] {
            return observer.frames > start && observer.parsing.empty() && observer.idleFrame;
        };
        while (std::chrono::steady_clock::now() < deadline) {
            loop.runOnce();
            if (quiet()) {
                // Still quiet a little later: no answer due on the file source's 1 ms tick is left.
                const auto settled = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
                while (std::chrono::steady_clock::now() < settled) {
                    loop.runOnce();
                }
                if (quiet()) {
                    return;
                }
            }
        }
        ADD_FAILURE() << "the map did not settle within 30 s";
    }

    void release(const CanonicalTileID& id) {
        rules[id].holdNetwork = false;
        settle();
    }

    PremultipliedImage image() {
        gfx::BackendScope guard{*frontend.getBackend()};
        return frontend.readStillImage();
    }

    std::optional<double> elevation(const LatLng& point) {
        return frontend.getRenderer()->queryTerrainElevation(point);
    }
};

// The DEM tiles round the corner: north-west, north-east, south-west, south-east.
const CanonicalTileID northWest(demMaxZoom, cornerX - 1, cornerY - 1);
const CanonicalTileID northEast(demMaxZoom, cornerX, cornerY - 1);
const CanonicalTileID southWest(demMaxZoom, cornerX - 1, cornerY);
const CanonicalTileID southEast(demMaxZoom, cornerX, cornerY);

// Points within half a DEM pixel either side of the two seams through the corner (where the
// height is read from the border), along the stretch of each in view: the north-south seam
// north of the corner and the east-west seam either side of it.
std::vector<LatLng> seamPoints() {
    const LatLng origin = corner();
    const double n = std::pow(2.0, demMaxZoom) * demSize;
    const double lngPerPixel = 360.0 / n;
    // A DEM pixel's height in latitude, near enough at the corner (Mercator over one pixel).
    const double latPerPixel = lngPerPixel * std::cos(util::deg2rad(origin.latitude()));
    std::vector<LatLng> points;
    for (const double pixels : {-0.5, -0.25, 0.0, 0.25, 0.5}) {
        for (const double alongKm : {0.3, 1.0, 2.0}) {
            points.emplace_back(origin.latitude() + alongKm / kmPerDegree, origin.longitude() + pixels * lngPerPixel);
        }
        for (const double alongKm : {-1.0, -0.3, 0.3, 1.0}) {
            const double kmPerLng = kmPerDegree * std::cos(util::deg2rad(origin.latitude()));
            points.emplace_back(origin.latitude() + pixels * latPerPixel, origin.longitude() + alongKm / kmPerLng);
        }
    }
    return points;
}

// The elevation query reads the plane at every seam point: the DEM's bilinear surface is the
// plane where the borders hold the neighbours' pixels, to the encoding's 0.1 m steps.
void expectSeamOnThePlane(SeamScene& scene, const std::string& when) {
    for (const auto& point : seamPoints()) {
        const auto meters = scene.elevation(point);
        ASSERT_TRUE(meters) << when << ": no elevation at " << point.latitude() << ", " << point.longitude();
        EXPECT_NEAR(*meters, planeMeters(point) * exaggeration, 0.25 * exaggeration)
            << when << ": at " << point.latitude() << ", " << point.longitude();
    }
}

// How many pixels of `image` differ from `reference` by more than `tolerance` in any channel.
int differingPixels(const PremultipliedImage& image, const PremultipliedImage& reference, int tolerance = 8) {
    if (image.size != reference.size) {
        return -1;
    }
    int count = 0;
    const size_t n = static_cast<size_t>(image.size.width) * image.size.height;
    for (size_t i = 0; i < n; ++i) {
        for (size_t c = 0; c < 4; ++c) {
            if (std::abs(int{image.data[i * 4 + c]} - int{reference.data[i * 4 + c]}) > tolerance) {
                ++count;
                break;
            }
        }
    }
    return count;
}

// The frame a map draws over the corner when every DEM tile arrives at once.
const PremultipliedImage& referenceFrame() {
    static const PremultipliedImage reference = [] {
        SeamScene scene;
        return scene.image();
    }();
    return reference;
}

// Call referenceFrame() before making a scene: the reference's own scene runs its own run loop.
void expectFrameAsLoadedAtOnce(SeamScene& scene, const std::string& when) {
    EXPECT_EQ(differingPixels(scene.image(), referenceFrame()), 0)
        << when << ": the frame differs from the one drawn when every DEM tile arrived at once";
}

} // namespace

// A map that loads every DEM tile at once reads the plane at the seams: the baseline for the rest.
TEST(TerrainDEMSeam, HoldsWhenEveryTileArrivesAtOnce) {
    referenceFrame();
    SeamScene scene;
    expectSeamOnThePlane(scene, "at rest");
    EXPECT_EQ(differingPixels(scene.image(), referenceFrame()), 0);
}

// The flight in the app: every tile leaves the view, waits in the tile cache, and is loaded and
// parsed again when the camera comes back.
TEST(TerrainDEMSeam, HoldsAfterTheTilesLeaveTheViewAndComeBack) {
    referenceFrame();
    SeamScene scene;
    expectSeamOnThePlane(scene, "at rest");
    const int loadsBefore = scene.networkLoads[southEast];
    scene.lookAt({corner().latitude(), corner().longitude() + 3.0});
    scene.lookAt(corner());
    ASSERT_GT(scene.networkLoads[southEast], loadsBefore) << "the tile was not loaded again on the way back";
    expectSeamOnThePlane(scene, "after the flight");
    expectFrameAsLoadedAtOnce(scene, "after the flight");
}

// The same flight with the tile cache off: every tile is dropped and made anew on the way back.
TEST(TerrainDEMSeam, HoldsAfterTheTilesAreDroppedAndMadeAgain) {
    referenceFrame();
    SeamScene scene;
    scene.frontend.getRenderer()->setTileCacheEnabled(false);
    scene.lookAt({corner().latitude(), corner().longitude() + 3.0});
    scene.lookAt(corner());
    expectSeamOnThePlane(scene, "after the flight");
    expectFrameAsLoadedAtOnce(scene, "after the flight");
}

// A neighbour that arrives after the tile, which has been drawn without it: the tile's border
// is backfilled then, and the mesh drawn from the tile's DEM texture must take it.
TEST(TerrainDEMSeam, HoldsWhenANeighbourArrivesAfterTheTile) {
    referenceFrame();
    for (const auto& late : {southEast, northEast, northWest}) {
        SeamScene scene({{late, {.cachedOffset = std::nullopt, .holdNetwork = true}}});
        scene.release(late);
        expectSeamOnThePlane(scene, "after the late neighbour");
        expectFrameAsLoadedAtOnce(scene, "after the late neighbour");
    }
}

// A tile parsed again while its neighbours are kept: from the cache first, then from the network
// (DefaultFileSource does this for every tile it has cached), here once the neighbours are in.
TEST(TerrainDEMSeam, HoldsWhenATileIsParsedAgainWhileItsNeighboursAreKept) {
    referenceFrame();
    SeamScene scene({{northEast, {.cachedOffset = 0.0, .holdNetwork = true}}});
    expectSeamOnThePlane(scene, "with the cached copy");
    scene.release(northEast);
    ASSERT_EQ(scene.networkLoads[northEast], 1);
    expectSeamOnThePlane(scene, "after the network copy");
    expectFrameAsLoadedAtOnce(scene, "after the network copy");
}

// The same, with a stale cached copy 40 m below the network's: the neighbours' borders, backfilled
// from the cached copy, take the new heights too.
TEST(TerrainDEMSeam, HoldsWhenATileIsReloadedWithNewHeights) {
    referenceFrame();
    SeamScene scene({{northEast, {.cachedOffset = -40.0, .holdNetwork = true}}});
    scene.release(northEast);
    ASSERT_EQ(scene.networkLoads[northEast], 1);
    expectSeamOnThePlane(scene, "after the network copy");
    expectFrameAsLoadedAtOnce(scene, "after the network copy");
}

// Neighbours that have not arrived, with their parent standing in for them from the cache (a
// tile's border is backfilled only from tiles of its own zoom, as in maplibre-gl-js), then arrive.
TEST(TerrainDEMSeam, HoldsWhenNeighboursArriveAfterTheirParentStoodIn) {
    referenceFrame();
    const CanonicalTileID parent(demMaxZoom - 1, cornerX / 2, cornerY / 2);
    SeamScene scene({{southEast, {.cachedOffset = std::nullopt, .holdNetwork = true}},
                     {southWest, {.cachedOffset = std::nullopt, .holdNetwork = true}},
                     {parent, {.cachedOffset = 0.0, .holdNetwork = true}}});
    ASSERT_TRUE(scene.elevation({corner().latitude() - 0.01, corner().longitude() + 0.01}))
        << "no DEM stands in for the tiles still on their way";
    scene.rules[southEast].holdNetwork = false;
    scene.release(southWest);
    expectSeamOnThePlane(scene, "after the neighbours");
    expectFrameAsLoadedAtOnce(scene, "after the neighbours");
}
