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
#include <mln/style/image.hpp>
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

// Where a cached copy's bump stands: 0.6 km north and 0.3 km east of the corner, inside the
// north-east tile, 0.4 km wide. Well inside the tile, so it leaves the tile's lowest and highest
// heights (and so the tile covers drawn from them) as they are.
constexpr double bumpNorthKm = 0.6;
constexpr double bumpEastKm = 0.3;
constexpr double bumpWidthKm = 0.2;

double bumpMeters(const LatLng& point, double height) {
    const LatLng origin = corner();
    const double northKm = (point.latitude() - origin.latitude()) * kmPerDegree - bumpNorthKm;
    const double eastKm = (point.longitude() - origin.longitude()) * kmPerDegree *
                              std::cos(util::deg2rad(origin.latitude())) -
                          bumpEastKm;
    return height * std::exp(-(northKm * northKm + eastKm * eastKm) / (bumpWidthKm * bumpWidthKm));
}

// The DEM tile z/x/y of the plane, `offset` metres up and with a bump `bump` metres high, each
// pixel holding the height at its centre (where the samplers place it), in Mapbox Terrain-RGB.
std::string planeTile(const CanonicalTileID& id, double offset, double bump = 0.0) {
    const double n = std::pow(2.0, id.z);
    PremultipliedImage image({demSize, demSize});
    for (uint32_t row = 0; row < demSize; ++row) {
        const double mercatorY = (id.y + (row + 0.5) / demSize) / n;
        const double latitude = util::rad2deg(std::atan(std::sinh(M_PI * (1.0 - 2.0 * mercatorY))));
        for (uint32_t col = 0; col < demSize; ++col) {
            const double longitude = (id.x + (col + 0.5) / demSize) / n * 360.0 - 180.0;
            const auto value = static_cast<uint32_t>(std::lround(
                (planeMeters({latitude, longitude}) + offset + bumpMeters({latitude, longitude}, bump) + 10000.0) *
                10.0));
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

// What a scene draws, besides the grid draped over the terrain.
struct SceneOptions {
    double zoom = 14.0;
    double pitch = 50.0;
    /// The layer drawn from the DEM itself, draped.
    std::string demLayer = R"({"id": "hillshade", "type": "hillshade", "source": "dem"})";
    /// GeoJSON points (a geometry) drawn on the terrain as `pointLayer` draws them.
    std::string points;
    /// A layer of source "points": its JSON. The style image "dot" (a red square) is added.
    std::string pointLayer;
    /// Whether every DEM tile's answer is held back, cache and network, so none ever arrives.
    bool holdEveryDEMTile = false;
};

std::string seamStyle(const SceneOptions& options) {
    const std::string points = options.points.empty()
                                   ? ""
                                   : R"(, "points": {"type": "geojson", "data": )" + options.points + "}";
    const std::string pointLayer = options.pointLayer.empty() ? "" : ", " + options.pointLayer;
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
           gridGeoJSON() + "}" + points + R"STYLE(
  },
  "terrain": {"source": "dem", "exaggeration": )STYLE" +
           std::to_string(exaggeration) + R"STYLE(},
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "white"}},
    )STYLE" +
           options.demLayer +
           R"STYLE(,
    {"id": "grid", "type": "line", "source": "grid", "paint": {"line-color": "black", "line-width": 2}}
  )STYLE" + pointLayer +
           R"STYLE(]
})STYLE";
}

// How the file source answers one DEM tile: what the offline cache holds (DefaultFileSource
// answers a tile from the cache first, then from the network), whether the network answer is
// held back for now, and the network copy's own heights.
struct TileRule {
    /// Metres the cached copy is off the plane; no cached copy when empty.
    std::optional<double> cachedOffset;
    bool holdNetwork = false;
    /// Metres the network copy is off the plane: a cliff at the tile's edges.
    double networkOffset = 0.0;
    /// The height of a bump on the cached copy (see bumpMeters).
    double cachedBump = 0.0;
    /// The height of a bump on the network copy.
    double networkBump = 0.0;
};

// A tile whose network answer is held back, with this cached copy (none by default). Built field
// by field: clang warns when designated initializers leave fields out.
TileRule held(std::optional<double> cachedOffset = std::nullopt) {
    TileRule rule;
    rule.cachedOffset = cachedOffset;
    rule.holdNetwork = true;
    return rule;
}

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
    SceneOptions options;
    /// Network answers with data per DEM tile.
    std::map<CanonicalTileID, int> networkLoads;
    /// Whether the network answers a tile it has sent before 304 Not Modified, as a server
    /// revalidating the copy the map holds does.
    bool notModified = false;
    int notModifiedLoads = 0;

    explicit SeamScene(std::map<CanonicalTileID, TileRule> rules_ = {}, SceneOptions options_ = {})
        : rules(std::move(rules_)),
          options(std::move(options_)) {
        fileSource->tileResponse = [this](const Resource& resource) -> std::optional<Response> {
            Response res;
            if (!resource.tileData || resource.url.find("/dem/") == std::string::npos) {
                res.noContent = true;
                return res;
            }
            const CanonicalTileID id(
                resource.tileData->z, static_cast<uint32_t>(resource.tileData->x), resource.tileData->y);
            if (options.holdEveryDEMTile) {
                if (resource.loadingMethod == Resource::LoadingMethod::CacheOnly) {
                    res.error = std::make_unique<Response::Error>(Response::Error::Reason::NotFound, "not cached");
                    return res;
                }
                return std::nullopt; // still on its way
            }
            const auto rule = rules.find(id);
            if (resource.loadingMethod == Resource::LoadingMethod::CacheOnly) {
                if (rule == rules.end() || !rule->second.cachedOffset) {
                    res.error = std::make_unique<Response::Error>(Response::Error::Reason::NotFound, "not cached");
                    return res;
                }
                res.data = std::make_shared<std::string>(
                    planeTile(id, *rule->second.cachedOffset, rule->second.cachedBump));
                return res;
            }
            if (rule != rules.end() && rule->second.holdNetwork) {
                return std::nullopt; // still on its way
            }
            if (notModified && networkLoads[id] > 0) {
                ++notModifiedLoads;
                res.notModified = true;
                return res;
            }
            ++networkLoads[id];
            res.data = std::make_shared<std::string>(
                rule != rules.end() ? planeTile(id, rule->second.networkOffset, rule->second.networkBump)
                                    : planeTile(id, 0.0));
            return res;
        };
        map.getStyle().loadJSON(seamStyle(options));
        if (!options.pointLayer.empty()) {
            PremultipliedImage dot({8, 8});
            for (size_t i = 0; i < dot.bytes(); i += 4) {
                dot.data[i] = 255;
                dot.data[i + 1] = 0;
                dot.data[i + 2] = 0;
                dot.data[i + 3] = 255;
            }
            map.getStyle().addImage(std::make_unique<style::Image>("dot", std::move(dot), 1.0f));
        }
        lookAt(corner());
    }

    void lookAt(const LatLng& center) {
        map.jumpTo(CameraOptions()
                       .withCenter(center)
                       .withZoom(options.zoom)
                       .withPitch(options.pitch)
                       .withBearing(0.0)
                       .withCenterAltitude(planeMeters(center) * exaggeration));
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
        SeamScene scene({{late, held()}});
        scene.release(late);
        expectSeamOnThePlane(scene, "after the late neighbour");
        expectFrameAsLoadedAtOnce(scene, "after the late neighbour");
    }
}

// A tile parsed again while its neighbours are kept: from the cache first, then from the network
// (DefaultFileSource does this for every tile it has cached), here once the neighbours are in.
TEST(TerrainDEMSeam, HoldsWhenATileIsParsedAgainWhileItsNeighboursAreKept) {
    referenceFrame();
    SeamScene scene({{northEast, held(0.0)}});
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
    SeamScene scene({{northEast, held(-40.0)}});
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
    SeamScene scene({{southEast, held()}, {southWest, held()}, {parent, held(0.0)}});
    ASSERT_TRUE(scene.elevation({corner().latitude() - 0.01, corner().longitude() + 0.01}))
        << "no DEM stands in for the tiles still on their way";
    scene.rules[southEast].holdNetwork = false;
    scene.release(southWest);
    expectSeamOnThePlane(scene, "after the neighbours");
    expectFrameAsLoadedAtOnce(scene, "after the neighbours");
}

namespace {

// The frame a map with these options draws when every DEM tile arrives at once, with the heights
// the network sends under these rules (no cached copies, nothing held). Make it before the scene
// it is compared with: each scene runs its own run loop.
PremultipliedImage frameLoadedAtOnce(std::map<CanonicalTileID, TileRule> rules, const SceneOptions& options) {
    for (auto& [id, rule] : rules) {
        TileRule fresh;
        fresh.networkOffset = rule.networkOffset;
        fresh.networkBump = rule.networkBump;
        rule = fresh;
    }
    SeamScene scene(std::move(rules), options);
    return scene.image();
}

std::string multiPoint(const std::vector<LatLng>& points) {
    std::string coordinates;
    for (const auto& point : points) {
        coordinates += (coordinates.empty() ? "[" : ",[") + std::to_string(point.longitude()) + "," +
                       std::to_string(point.latitude()) + "]";
    }
    return R"({"type": "MultiPoint", "coordinates": [)" + coordinates + "]}";
}

} // namespace

// A tile coming back from the tile cache that the server answers 304 Not Modified: it is not
// parsed again, so its border is the one its neighbours kept up to date while it was away.
TEST(TerrainDEMSeam, HoldsWhenTheTilesComeBackNotModified) {
    referenceFrame();
    SeamScene scene;
    scene.lookAt({corner().latitude(), corner().longitude() + 3.0});
    scene.notModified = true;
    scene.lookAt(corner());
    ASSERT_GT(scene.notModifiedLoads, 0) << "no tile was revalidated on the way back";
    expectSeamOnThePlane(scene, "after a 304");
    expectFrameAsLoadedAtOnce(scene, "after a 304");
}

// How many pixels of `image` are the red of the style image "dot".
int redPixels(const PremultipliedImage& image) {
    int count = 0;
    const size_t n = static_cast<size_t>(image.size.width) * image.size.height;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* px = image.data.get() + i * 4;
        if (px[0] > 200 && px[1] < 60 && px[2] < 60) {
            ++count;
        }
    }
    return count;
}

// Icons drawn while no DEM tile has arrived: the terrain has no mesh yet, so the depth pass has
// nothing to draw and never runs. The icons must test against the far plane then, not against the
// depth target, whose texture nothing has drawn into and which has no image yet: Vulkan
// dereferenced its missing image view when binding it (a segfault whenever the icons' tile was
// drawn before the first DEM tile), and Metal sampled nothing there, which hid every icon.
TEST(TerrainDEMSeam, ShowsTheIconsWhileNoDEMTileHasArrived) {
    const LatLng origin = corner();
    const double kmPerLng = kmPerDegree * std::cos(util::deg2rad(origin.latitude()));
    std::vector<LatLng> icons;
    for (const double northKm : {-0.2, 0.0, 0.2}) {
        for (const double eastKm : {-0.2, 0.0, 0.2}) {
            icons.emplace_back(origin.latitude() + northKm / kmPerDegree, origin.longitude() + eastKm / kmPerLng);
        }
    }
    SceneOptions options;
    options.holdEveryDEMTile = true;
    options.points = multiPoint(icons);
    options.pointLayer = R"({"id": "icons", "type": "symbol", "source": "points",
        "layout": {"icon-image": "dot", "icon-allow-overlap": true, "icon-ignore-placement": true}})";
    SeamScene scene({}, options);
    ASSERT_TRUE(scene.networkLoads.empty()) << "a DEM tile arrived";
    ASSERT_FALSE(scene.elevation(origin)) << "the terrain has heights, so a DEM tile arrived";
    // Nine 8 px squares; a few edge pixels may blend.
    EXPECT_GE(redPixels(scene.image()), 9 * 40) << "the icons are hidden while no DEM tile has arrived";
}

// The packed terrain depth, against which symbols test whether the ground hides them: a tile whose
// network copy has a 300 m bump its cached copy lacked must draw the depth again, or the icons
// standing just behind and on the bump are tested against ground that is no longer there. Seen
// from 70° pitch, so the bump stands up into the icons' line of sight. The bump leaves the tile's
// height range, and so the terrain cover, as it was: only the DEM changes, and the depth would be
// drawn again for nothing else.
TEST(TerrainDEMSeam, HoldsInTheTerrainDepthWhenATileIsReloadedWithNewHeights) {
    const LatLng origin = corner();
    const double kmPerLng = kmPerDegree * std::cos(util::deg2rad(origin.latitude()));
    std::vector<LatLng> icons;
    for (const double northKm : {0.4, 0.5, 0.6}) {
        for (const double eastKm : {-0.1, 0.0, 0.1}) {
            icons.emplace_back(origin.latitude() + (bumpNorthKm + northKm) / kmPerDegree,
                               origin.longitude() + (bumpEastKm + eastKm) / kmPerLng);
        }
    }
    SceneOptions options;
    options.pitch = 70.0;
    options.points = multiPoint(icons);
    options.pointLayer = R"({"id": "icons", "type": "symbol", "source": "points",
        "layout": {"icon-image": "dot", "icon-allow-overlap": true, "icon-ignore-placement": true}})";
    TileRule bumped = held(0.0);
    bumped.networkBump = 300.0;
    const std::map<CanonicalTileID, TileRule> rules{{northEast, bumped}};
    const PremultipliedImage reference = frameLoadedAtOnce(rules, options);
    SeamScene scene(rules, options);
    scene.release(northEast);
    ASSERT_EQ(scene.networkLoads[northEast], 1);
    EXPECT_EQ(differingPixels(scene.image(), reference), 0)
        << "the frame (icons hidden by the terrain depth) differs from the one drawn when every DEM tile "
           "arrived at once";
}

// The color relief samples the DEM directly; a border backfilled after its drape was rendered
// must render the drape again. Bands of colour every 20 m, so metres off at the seam show.
TEST(TerrainDEMSeam, HoldsInTheColorReliefWhenANeighbourArrivesAfterTheTile) {
    std::string stops;
    for (int meters = 0; meters <= 8000; meters += 20) {
        stops += ", " + std::to_string(meters) + (meters % 40 == 0 ? R"(, "blue")" : R"(, "yellow")");
    }
    SceneOptions options;
    options.demLayer = R"({"id": "relief", "type": "color-relief", "source": "dem",
        "paint": {"color-relief-color": ["interpolate", ["linear"], ["elevation"])" +
                       stops + "]}}";
    const std::map<CanonicalTileID, TileRule> rules{{southEast, held()}};
    const PremultipliedImage reference = frameLoadedAtOnce(rules, options);
    SeamScene scene(rules, options);
    scene.release(southEast);
    EXPECT_EQ(differingPixels(scene.image(), reference), 0)
        << "the color relief differs from the one drawn when every DEM tile arrived at once";
}

// Below the DEM's max zoom a 512 px GeoJSON tile spans four 256 px DEM tiles, and the layers that
// stand on the terrain sample a DEM stitched from them, whose outer ring comes from their borders.
// A neighbour outside the stitched tile reloaded after the stitch (its network copy a cliff
// 2,000 m above its cached one, so the ring's change shows) backfills a child's border in place,
// which must stitch the DEM again: circles a quarter of a DEM pixel inside the stitched tile's
// north edge stand on a quarter of the cliff.
TEST(TerrainDEMSeam, HoldsInAStitchedDEMWhenANeighbourIsReloadedAfterTheStitch) {
    const LatLng origin = corner();
    const double lngPerPixel = 360.0 / (std::pow(2.0, demMaxZoom) * demSize);
    const double latPerPixel = lngPerPixel * std::cos(util::deg2rad(origin.latitude()));
    std::vector<LatLng> circles;
    for (const double pixels : {40.0, 128.0, 216.0}) {
        circles.emplace_back(origin.latitude() - 0.25 * latPerPixel, origin.longitude() + pixels * lngPerPixel);
    }
    SceneOptions options;
    options.zoom = 10.4;
    options.points = multiPoint(circles);
    options.pointLayer = R"({"id": "circles", "type": "circle", "source": "points",
        "paint": {"circle-radius": 5, "circle-color": "red"}})";
    TileRule cliff = held(0.0);
    cliff.networkOffset = 2000.0;
    const std::map<CanonicalTileID, TileRule> rules{{northEast, cliff}};
    const PremultipliedImage reference = frameLoadedAtOnce(rules, options);
    SeamScene scene(rules, options);
    scene.release(northEast);
    EXPECT_EQ(differingPixels(scene.image(), reference), 0)
        << "the circles on the stitched DEM differ from those drawn when every DEM tile arrived at once";
}
