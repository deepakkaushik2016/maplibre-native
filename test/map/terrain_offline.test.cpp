#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/storage/main_resource_loader.hpp>
#include <mln/storage/network_status.hpp>
#include <mln/storage/offline.hpp>
#include <mln/storage/offline_database.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/style/style.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/util/geo.hpp>
#include <mln/util/image.hpp>
#include <mln/util/io.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/tile_server_options.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace mln;
using namespace std::literals::string_literals;

// Offline hiking: a downloaded region can hold the basemap but miss some DEM tiles (a partial
// download, a tile the server never had), and offline every DEM tile comes from the offline
// database. Terrain must then render flat and textured where the DEM is missing - never black,
// never sunk - and a DEM read from the offline database must render exactly like the same DEM
// fetched online.

namespace {

constexpr uint32_t viewSize = 512;
constexpr double plateauMeters = 300.0;
// The DEM's maxzoom. The camera's centre sits on a corner shared by four z11 tiles, so the four
// geographic quadrants around it are four different DEM tiles (and every lower-zoom tile that
// contains the centre straddles quadrants).
constexpr int demMaxZoom = 11;
const CanonicalTileID neTile{demMaxZoom, 1081, 728};
// The centre: the south-west corner of neTile, near 46 N 10 E.
const LatLng center = LatLngBounds(neTile).southwest();
// z12 at pitch 45: the eye is several km above the 300 m plateau, so the frame is not blanked by
// the camera sitting under the terrain (the centre altitude is not anchored yet, TERRAIN.md
// phase 4; that only bites from about z13).
constexpr double zoom = 12.0;
constexpr double pitch = 45.0;

const std::string demURL = "http://tiles.invalid/dem/{z}/{x}/{y}.png";
const std::string baseURL = "http://tiles.invalid/base/{z}/{x}/{y}.png";
const std::string styleURL = "http://tiles.invalid/style.json";

std::string sceneStyle(bool withTerrain, const std::string& encoding = "mapbox", int demZoomMax = 11) {
    return R"STYLE({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": [")STYLE" +
           demURL + R"STYLE("],
      "encoding": ")STYLE" +
           encoding + R"STYLE(",
      "maxzoom": )STYLE" +
           std::to_string(demZoomMax) + R"STYLE(,
      "tileSize": 256
    },
    "base": {
      "type": "raster",
      "tiles": [")STYLE" +
           baseURL + R"STYLE("],
      "maxzoom": 14,
      "tileSize": 256
    }
  },)STYLE" +
           (withTerrain ? R"STYLE("terrain": {"source": "dem", "exaggeration": 1},)STYLE"s : ""s) + R"STYLE(
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "#f4f1ea"}},
    {"id": "base", "type": "raster", "source": "base", "paint": {"raster-fade-duration": 0}}
  ]
})STYLE";
}

std::string plateauTile(bool terrarium = false) {
    // Terrain-RGB: -10000 + (R * 65536 + G * 256 + B) * 0.1 = 300 m for RGB(1, 146, 88).
    // Terrarium: R * 256 + G + B / 256 - 32768 = 300 m for RGB(129, 44, 0).
    const std::array<uint8_t, 3> rgb = terrarium ? std::array<uint8_t, 3>{{129, 44, 0}}
                                                 : std::array<uint8_t, 3>{{1, 146, 88}};
    PremultipliedImage image({256, 256});
    for (uint32_t i = 0; i < 256 * 256; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        px[0] = rgb[0];
        px[1] = rgb[1];
        px[2] = rgb[2];
        px[3] = 255;
    }
    return encodePNG(image);
}

// The draped basemap: four 128 px blocks per tile, the top-left one transparent so the style's
// background shows through. Blocks give the frame structure, so a surface drawn in the wrong
// place (sunk, lifted) fails the pixel comparison against the 2D render instead of passing on a
// uniform colour.
std::string baseTile() {
    PremultipliedImage image({256, 256});
    for (uint32_t y = 0; y < 256; ++y) {
        for (uint32_t x = 0; x < 256; ++x) {
            uint8_t* px = image.data.get() + (y * 256 + x) * 4;
            std::array<uint8_t, 4> c{{0, 0, 0, 0}};
            if (x >= 128 && y < 128) c = {{42, 122, 176, 255}};  // blue
            if (x < 128 && y >= 128) c = {{51, 170, 119, 255}};  // green
            if (x >= 128 && y >= 128) c = {{204, 136, 68, 255}}; // orange
            for (int i = 0; i < 4; ++i) px[i] = c[i];
        }
    }
    return encodePNG(image);
}

// A DEM tile at zoom <= demMaxZoom lies wholly in the quadrant north-east of the centre.
bool inNorthEastQuadrant(const CanonicalTileID& id) {
    const int shift = demMaxZoom - id.z;
    if (shift < 0) return false;
    const int64_t x0 = static_cast<int64_t>(id.x) << shift;
    const int64_t y1 = (static_cast<int64_t>(id.y) + 1) << shift;
    return std::cmp_greater_equal(x0, neTile.x) && std::cmp_less_equal(y1, neTile.y + 1);
}

Response dataResponse(const std::string& data) {
    Response res;
    res.data = std::make_shared<std::string>(data);
    return res;
}

Response notFound() {
    Response res;
    res.noContent = true;
    res.error = std::make_unique<Response::Error>(Response::Error::Reason::NotFound, "DEM tile not found");
    return res;
}

const uint8_t* pixel(const PremultipliedImage& image, uint32_t x, uint32_t y) {
    return image.data.get() + (static_cast<size_t>(y) * image.size.width + x) * 4;
}

bool isPureBlack(const PremultipliedImage& image, uint32_t x, uint32_t y) {
    const uint8_t* px = pixel(image, x, y);
    return px[0] == 0 && px[1] == 0 && px[2] == 0;
}

// CIE76 delta E between two sRGB pixels (opaque here: the background covers every pixel).
std::array<double, 3> lab(const uint8_t* px) {
    auto lin = [](uint8_t v) {
        const double c = v / 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    const double r = lin(px[0]), g = lin(px[1]), b = lin(px[2]);
    const double x = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047;
    const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    const double z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883;
    auto f = [](double t) {
        return t > 216.0 / 24389.0 ? std::cbrt(t) : (24389.0 / 27.0 * t + 16.0) / 116.0;
    };
    return {{116.0 * f(y) - 16.0, 500.0 * (f(x) - f(y)), 200.0 * (f(y) - f(z))}};
}

double deltaE(const uint8_t* a, const uint8_t* b) {
    const auto la = lab(a);
    const auto lb = lab(b);
    return std::sqrt(std::pow(la[0] - lb[0], 2) + std::pow(la[1] - lb[1], 2) + std::pow(la[2] - lb[2], 2));
}

// Screen quadrant of a pixel; at bearing 0 the centre's meridian is the middle column and its
// parallel the middle row, so the screen's top-right quadrant is the one with DEM.
bool inScreenNorthEast(uint32_t x, uint32_t y) {
    return x >= viewSize / 2 && y < viewSize / 2;
}

// The test owns the run loop (util::RunLoop): the file sources are created before the scenes and
// outlive them, so they must all live on one loop that outlives everything else.
struct Scene {
    std::shared_ptr<FileSource> fileSource;
    HeadlessFrontend frontend{{viewSize, viewSize}, 1};
    MapAdapter map;

    explicit Scene(std::shared_ptr<FileSource> fs)
        : fileSource(std::move(fs)),
          map(frontend,
              MapObserver::nullObserver(),
              fileSource,
              MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())) {}

    void jump(double centerAltitude) {
        map.jumpTo(
            CameraOptions().withCenter(center).withZoom(zoom).withPitch(pitch).withCenterAltitude(centerAltitude));
    }

    // One still frame; it waits for every tile the frame needs.
    PremultipliedImage render() { return frontend.render(map).image; }

    std::optional<double> elevation(const LatLng& at) { return frontend.getRenderer()->queryTerrainElevation(at); }
};

// Points well inside each geographic quadrant (a quarter of a z11 tile from the centre).
const LatLngBounds neBounds(neTile);
const double dLat = (neBounds.north() - neBounds.south()) / 4;
const double dLng = (neBounds.east() - neBounds.west()) / 4;
const LatLng inNE{center.latitude() + dLat, center.longitude() + dLng};
const LatLng inSW{center.latitude() - dLat, center.longitude() - dLng};

} // namespace

namespace {

void expectMissingDEMRendersTexturedFlat(const std::string& encoding) {
    util::RunLoop loop;
    const std::string dem = plateauTile(encoding == "terrarium");
    const std::string base = baseTile();
    int demServed = 0;
    int demMissing = 0;

    auto stub = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    stub->tileResponse = [&](const Resource& resource) -> std::optional<Response> {
        const auto& t = *resource.tileData;
        if (t.urlTemplate == baseURL) return dataResponse(base);
        if (inNorthEastQuadrant(CanonicalTileID(t.z, t.x, t.y))) {
            ++demServed;
            return dataResponse(dem);
        }
        ++demMissing;
        return notFound();
    };

    // The 2D render of the same camera: the reference the flat, draped quadrants must match.
    PremultipliedImage flat;
    {
        Scene scene(stub);
        scene.map.getStyle().loadJSON(sceneStyle(false, encoding));
        scene.jump(0.0);
        flat = scene.render();
    }

    Scene scene(stub);
    scene.map.getStyle().loadJSON(sceneStyle(true, encoding));
    scene.jump(0.0);
    const PremultipliedImage image = scene.render();
    ASSERT_EQ(image.size, (Size{viewSize, viewSize}));
    ASSERT_EQ(flat.size, image.size);

    // The scene is the one intended: some DEM tiles served, others missing, and the terrain is up
    // where the DEM is (so the frame is a terrain frame) and absent elsewhere.
    ASSERT_GT(demServed, 0) << demMissing << " DEM tiles missing";
    ASSERT_GT(demMissing, 0) << demServed << " DEM tiles served";
    const auto neElevation = scene.elevation(inNE);
    ASSERT_TRUE(neElevation) << "the north-east DEM never loaded";
    EXPECT_NEAR(*neElevation, plateauMeters, 1.0);
    const auto swElevation = scene.elevation(inSW);
    SCOPED_TRACE("elevation NE " + std::to_string(*neElevation) + " m, SW " +
                 (swElevation ? std::to_string(*swElevation) + " m" : "none (no DEM)"));

    // The 2D frame is textured everywhere (all four basemap colours and the background), so a
    // match against it is a match against drape content, not against an empty frame.
    {
        int blue = 0, green = 0, orange = 0, background = 0;
        const uint8_t bg[4] = {0xf4, 0xf1, 0xea, 255};
        const uint8_t bl[4] = {42, 122, 176, 255}, gr[4] = {51, 170, 119, 255}, ora[4] = {204, 136, 68, 255};
        for (uint32_t y = 0; y < viewSize; ++y) {
            for (uint32_t x = 0; x < viewSize; ++x) {
                const uint8_t* px = pixel(flat, x, y);
                if (deltaE(px, bg) < 5) ++background;
                if (deltaE(px, bl) < 5) ++blue;
                if (deltaE(px, gr) < 5) ++green;
                if (deltaE(px, ora) < 5) ++orange;
            }
        }
        SCOPED_TRACE("2D frame: background " + std::to_string(background) + ", blue " + std::to_string(blue) +
                     ", green " + std::to_string(green) + ", orange " + std::to_string(orange) + " px");
        ASSERT_GT(background, 1000);
        ASSERT_GT(blue, 1000);
        ASSERT_GT(green, 1000);
        ASSERT_GT(orange, 1000);
    }

    // No pure black in the three quadrants without DEM (nor anywhere else, reported).
    int blackNoDEM = 0;
    int blackAll = 0;
    for (uint32_t y = 0; y < viewSize; ++y) {
        for (uint32_t x = 0; x < viewSize; ++x) {
            if (isPureBlack(image, x, y)) {
                ++blackAll;
                if (!inScreenNorthEast(x, y)) ++blackNoDEM;
            }
        }
    }
    EXPECT_EQ(blackNoDEM, 0) << "terrain with a missing DEM tile renders black (" << blackAll
                             << " pure black px in the whole frame)";

    // The drape's colour is present where the DEM is missing, at the place the 2D render puts it:
    // the surface there is flat at sea level, so it projects exactly as the 2D map does. 20
    // deterministic samples spread over the three quadrants, 32 px clear of the quadrant
    // boundaries (where the plateau's cliff and skirts are drawn).
    constexpr int samples = 20;
    constexpr uint32_t margin = 32;
    int matches = 0;
    std::ostringstream mismatches;
    uint32_t state = 12345;
    auto next = [&](uint32_t lo, uint32_t hi) {
        state = state * 1103515245u + 12345u;
        return lo + (state >> 8) % (hi - lo);
    };
    for (int i = 0; i < samples; ++i) {
        uint32_t x = 0;
        uint32_t y = 0;
        switch (i % 3) {
            case 0: // north-west (top-left)
                x = next(margin, viewSize / 2 - margin);
                y = next(margin, viewSize / 2 - margin);
                break;
            case 1: // south-west (bottom-left)
                x = next(margin, viewSize / 2 - margin);
                y = next(viewSize / 2 + margin, viewSize - margin);
                break;
            default: // south-east (bottom-right)
                x = next(viewSize / 2 + margin, viewSize - margin);
                y = next(viewSize / 2 + margin, viewSize - margin);
                break;
        }
        const uint8_t* a = pixel(image, x, y);
        const uint8_t* b = pixel(flat, x, y);
        const double dE = deltaE(a, b);
        if (dE <= 10.0) {
            ++matches;
        } else {
            mismatches << "\n  (" << x << ", " << y << "): terrain (" << int(a[0]) << ", " << int(a[1]) << ", "
                       << int(a[2]) << ") 2D (" << int(b[0]) << ", " << int(b[1]) << ", " << int(b[2]) << ") dE " << dE;
        }
    }
    EXPECT_GE(matches, 18) << "the drape where the DEM is missing does not match the 2D map; " << samples - matches
                           << " of " << samples << " samples off by more than dE 10:" << mismatches.str();
}

} // namespace

TEST(TerrainOffline, MissingDEMRendersTexturedFlat) {
    expectMissingDEMRendersTexturedFlat("mapbox");
}

// The flat fallback is encoded per DEM encoding: Terrarium's zero is a different texel.
TEST(TerrainOffline, MissingTerrariumDEMRendersTexturedFlat) {
    expectMissingDEMRendersTexturedFlat("terrarium");
}

TEST(TerrainOffline, DEMFromOfflineDatabase) {
    util::RunLoop loop;
    const std::string dem = plateauTile();
    const std::string base = baseTile();
    const std::string styleJSON = sceneStyle(true);

    // Online: every resource from a stub server, recording what the frame needed.
    std::list<std::tuple<Resource, Response>> served;
    auto stub = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    stub->styleResponse = [&](const Resource& resource) -> std::optional<Response> {
        auto res = dataResponse(styleJSON);
        served.emplace_back(resource, res);
        return res;
    };
    stub->tileResponse = [&](const Resource& resource) -> std::optional<Response> {
        auto res = dataResponse(resource.tileData->urlTemplate == demURL ? dem : base);
        served.emplace_back(resource, res);
        return res;
    };

    PremultipliedImage online;
    std::optional<double> onlineElevation;
    {
        Scene scene(stub);
        scene.map.getStyle().loadURL(styleURL);
        scene.jump(plateauMeters);
        online = scene.render();
        onlineElevation = scene.elevation(center);
    }
    ASSERT_TRUE(onlineElevation) << "the online DEM never loaded";
    ASSERT_NEAR(*onlineElevation, plateauMeters, 1.0);

    int demTiles = 0;
    int baseTiles = 0;
    for (const auto& [resource, response] : served) {
        if (resource.tileData) {
            ++(resource.tileData->urlTemplate == demURL ? demTiles : baseTiles);
        }
    }
    ASSERT_GT(demTiles, 0) << "the online frame requested no DEM tile";
    ASSERT_GT(baseTiles, 0) << "the online frame requested no basemap tile";

    // The offline database: one region for the view's tile range, holding exactly those resources.
    const std::string path = "test/fixtures/offline_database/terrain_offline.db";
    auto deleteDatabase = [&] {
        util::deleteFile(path);
        util::deleteFile(path + "-wal");
        util::deleteFile(path + "-journal");
    };
    deleteDatabase();
    {
        OfflineDatabase db(path, TileServerOptions::DefaultConfiguration());
        const LatLngBounds bounds = LatLngBounds::hull({center.latitude() - 0.2, center.longitude() - 0.2},
                                                       {center.latitude() + 0.2, center.longitude() + 0.2});
        auto region = db.createRegion(OfflineTilePyramidRegionDefinition(styleURL, bounds, 0, zoom, 1.0f, false),
                                      OfflineRegionMetadata{});
        ASSERT_TRUE(region);
        OfflineRegionStatus status;
        db.putRegionResources(region->getID(), served, status);
        ASSERT_EQ(status.completedResourceCount, served.size())
            << "offline region " << region->getID() << " holds " << status.completedResourceSize << " bytes";
    }

    // Offline: the engine's own loader on that database, with network access disabled. The host
    // is .invalid, so a request that did reach the network could not succeed either.
    struct OfflineGuard {
        OfflineGuard() { NetworkStatus::Set(NetworkStatus::Status::Offline); }
        ~OfflineGuard() { NetworkStatus::Set(NetworkStatus::Status::Online); }
    } offlineGuard;

    PremultipliedImage offline;
    std::optional<double> offlineElevation;
    {
        auto loader = std::make_shared<MainResourceLoader>(ResourceOptions().withCachePath(path), ClientOptions());
        Scene scene(loader);
        scene.map.getStyle().loadURL(styleURL);
        scene.jump(plateauMeters);
        offline = scene.render();
        offlineElevation = scene.elevation(center);
    }
    ASSERT_TRUE(offlineElevation) << "the DEM did not load from the offline database";
    EXPECT_NEAR(*offlineElevation, plateauMeters, 1.0);

    // The online frame is a textured terrain frame (non-blank), so equality is meaningful.
    ASSERT_EQ(online.size, (Size{viewSize, viewSize}));
    ASSERT_EQ(offline.size, online.size);
    int onlineBlack = 0;
    int differing = 0;
    for (uint32_t y = 0; y < viewSize; ++y) {
        for (uint32_t x = 0; x < viewSize; ++x) {
            const uint8_t* a = pixel(online, x, y);
            const uint8_t* b = pixel(offline, x, y);
            onlineBlack += isPureBlack(online, x, y) ? 1 : 0;
            bool differs = false;
            for (int c = 0; c < 4; ++c) {
                differs = differs || std::abs(int(a[c]) - int(b[c])) > 8;
            }
            differing += differs ? 1 : 0;
        }
    }
    const double fraction = static_cast<double>(differing) / (viewSize * viewSize);
    ASSERT_EQ(onlineBlack, 0) << "the online frame has pure black pixels";
    EXPECT_LE(fraction, 0.01) << "the offline render differs from the online one: " << differing << " px";

    deleteDatabase();
}

// A still frame offline over a partly downloaded region (backlog B9). A still frame waits for every
// tile it covers. Offline, a DEM tile that is neither in the offline database nor reachable used to
// wait forever on the network request that the offline network status holds until the network is
// back, so the still never finished. It must finish and draw what the database has: the tile's
// ancestor, overzoomed, or flat ground at 0 m where no ancestor exists either.
//
// Every still here is waited for at most stillLimit, so a hang fails the test instead of stalling
// the run. A still of this scene from the database takes well under a second on Metal and a few
// seconds on llvmpipe; a still that waits on the network never finishes at all.

namespace {

constexpr auto stillLimit = std::chrono::seconds(20);
// The DEM goes to z13, but the database has it only to z12: at zoom 13 every DEM tile in view is
// missing, and the z12 tiles above them are in the database.
constexpr int sparseDEMMaxZoom = 13;
constexpr double stillZoom = 13.0;

struct Still {
    bool finished = false;
    std::exception_ptr error;
    PremultipliedImage image;
};

// One still frame, waited for at most `limit`.
Still renderStillWithin(HeadlessFrontend& frontend, Map& map, std::chrono::milliseconds limit) {
    Still still;
    gfx::BackendScope guard{*frontend.getBackend()};
    bool done = false;
    map.renderStill([&](const std::exception_ptr& error) {
        still.error = error;
        if (!error) {
            still.image = frontend.readStillImage();
        }
        done = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done && std::chrono::steady_clock::now() < deadline) {
        util::RunLoop::Get()->runOnce();
    }
    still.finished = done;
    return still;
}

// The share of pixels that differ by more than 8 in a channel.
double differingFraction(const PremultipliedImage& a, const PremultipliedImage& b) {
    int differing = 0;
    for (uint32_t y = 0; y < viewSize; ++y) {
        for (uint32_t x = 0; x < viewSize; ++x) {
            const uint8_t* pa = pixel(a, x, y);
            const uint8_t* pb = pixel(b, x, y);
            bool differs = false;
            for (int c = 0; c < 4; ++c) {
                differs = differs || std::abs(int(pa[c]) - int(pb[c])) > 8;
            }
            differing += differs ? 1 : 0;
        }
    }
    return static_cast<double>(differing) / (viewSize * viewSize);
}

struct StillScene {
    std::shared_ptr<FileSource> fileSource;
    HeadlessFrontend frontend{{viewSize, viewSize}, 1};
    MapAdapter map;

    explicit StillScene(std::shared_ptr<FileSource> fs)
        : fileSource(std::move(fs)),
          map(frontend,
              MapObserver::nullObserver(),
              fileSource,
              MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())) {
        map.getStyle().loadURL(styleURL);
        map.jumpTo(
            CameraOptions().withCenter(center).withZoom(stillZoom).withPitch(pitch).withCenterAltitude(plateauMeters));
    }

    Still render() { return renderStillWithin(frontend, map, stillLimit); }

    std::optional<double> elevation(const LatLng& at) { return frontend.getRenderer()->queryTerrainElevation(at); }
};

// An online scene from a stub server, which answers a DEM tile with data where `demHasData` says so
// and with 204 (no data) elsewhere. Returns its frame, and records every answer that carried data:
// what an offline region holds for this view.
PremultipliedImage renderOnline(const std::function<bool(const CanonicalTileID&)>& demHasData,
                                std::list<std::tuple<Resource, Response>>& withData,
                                int& demRequestedWithoutData) {
    const std::string dem = plateauTile();
    const std::string base = baseTile();
    const std::string styleJSON = sceneStyle(true, "mapbox", sparseDEMMaxZoom);
    auto stub = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    stub->styleResponse = [&](const Resource& resource) -> std::optional<Response> {
        auto res = dataResponse(styleJSON);
        withData.emplace_back(resource, res);
        return res;
    };
    stub->tileResponse = [&](const Resource& resource) -> std::optional<Response> {
        const auto& t = *resource.tileData;
        if (t.urlTemplate == demURL && !demHasData(CanonicalTileID(t.z, t.x, t.y))) {
            ++demRequestedWithoutData;
            Response empty;
            empty.noContent = true;
            return empty;
        }
        auto res = dataResponse(t.urlTemplate == demURL ? dem : base);
        withData.emplace_back(resource, res);
        return res;
    };
    StillScene scene(stub);
    Still still = scene.render();
    EXPECT_TRUE(still.finished) << "the online still did not finish";
    EXPECT_FALSE(still.error);
    return std::move(still.image);
}

// Puts the resources in a new offline database at `path`, the way an offline region holds them.
void writeDatabase(const std::string& path, const std::list<std::tuple<Resource, Response>>& resources) {
    util::deleteFile(path);
    util::deleteFile(path + "-wal");
    util::deleteFile(path + "-journal");
    OfflineDatabase db(path, TileServerOptions::DefaultConfiguration());
    for (const auto& [resource, response] : resources) {
        db.put(resource, response);
    }
}

void deleteDatabase(const std::string& path) {
    util::deleteFile(path);
    util::deleteFile(path + "-wal");
    util::deleteFile(path + "-journal");
}

struct OfflineNetwork {
    OfflineNetwork() { NetworkStatus::Set(NetworkStatus::Status::Offline); }
    ~OfflineNetwork() { NetworkStatus::Set(NetworkStatus::Status::Online); }
    OfflineNetwork(const OfflineNetwork&) = delete;
    OfflineNetwork& operator=(const OfflineNetwork&) = delete;
};

} // namespace

TEST(TerrainOffline, AStillFrameOfflineDrawsTheAncestorOfAMissingDEMTile) {
    util::RunLoop loop;
    const auto upToZ12 = [](const CanonicalTileID& id) {
        return id.z <= 12;
    };

    // The reference: online, where the z13 DEM tiles answer 204, so the frame draws their z12
    // ancestors (the sparse-DEM fallback). The offline frame must match it.
    std::list<std::tuple<Resource, Response>> region;
    int demWithoutData = 0;
    const PremultipliedImage sparse = renderOnline(upToZ12, region, demWithoutData);
    ASSERT_GT(demWithoutData, 0) << "the view asked for no z13 DEM tile";

    // Not vacuous: the same view over flat ground (every DEM tile empty) looks different.
    std::list<std::tuple<Resource, Response>> unused;
    int allEmpty = 0;
    const PremultipliedImage flat = renderOnline([](const CanonicalTileID&) { return false; }, unused, allEmpty);
    ASSERT_GE(differingFraction(sparse, flat), 0.04) << "the plateau frame looks like flat ground";

    int z12 = 0;
    for (const auto& [resource, response] : region) {
        if (resource.tileData && resource.tileData->urlTemplate == demURL) {
            ASSERT_LE(resource.tileData->z, 12);
            z12 += resource.tileData->z == 12 ? 1 : 0;
        }
    }
    ASSERT_GT(z12, 0) << "the database holds no z12 DEM tile";

    const std::string path = "test/fixtures/offline_database/terrain_offline_still_ancestor.db";
    writeDatabase(path, region);
    {
        OfflineNetwork offline;
        auto loader = std::make_shared<MainResourceLoader>(ResourceOptions().withCachePath(path), ClientOptions());
        StillScene scene(loader);
        const auto start = std::chrono::steady_clock::now();
        const Still still = scene.render();
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                start);
        ASSERT_TRUE(still.finished) << "an offline still frame waited on a missing DEM tile for more than "
                                    << stillLimit.count() << " s";
        ASSERT_FALSE(still.error);
        RecordProperty("offlineStillMs", static_cast<int>(took.count()));

        const auto elevation = scene.elevation(center);
        ASSERT_TRUE(elevation) << "no DEM under the centre: the ancestor was not drawn";
        EXPECT_NEAR(*elevation, plateauMeters, 1.0);
        const double offSparse = differingFraction(still.image, sparse);
        const double offFlat = differingFraction(still.image, flat);
        RecordProperty("differsFromAncestorPermille", static_cast<int>(offSparse * 1000));
        RecordProperty("differsFromFlatPermille", static_cast<int>(offFlat * 1000));
        EXPECT_LE(offSparse, 0.01) << "the offline frame does not match the frame drawn from the ancestor";
        EXPECT_GE(offFlat, 0.04) << "the offline frame is flat";
    }
    deleteDatabase(path);
}

TEST(TerrainOffline, AStillFrameOfflineWithNoDEMAtAllIsFlat) {
    util::RunLoop loop;
    std::list<std::tuple<Resource, Response>> region;
    int demWithoutData = 0;
    const PremultipliedImage flat = renderOnline([](const CanonicalTileID&) { return false; }, region, demWithoutData);
    ASSERT_GT(demWithoutData, 0);
    for (const auto& [resource, response] : region) {
        ASSERT_FALSE(resource.tileData && resource.tileData->urlTemplate == demURL);
    }

    const std::string path = "test/fixtures/offline_database/terrain_offline_still_flat.db";
    writeDatabase(path, region);
    {
        OfflineNetwork offline;
        auto loader = std::make_shared<MainResourceLoader>(ResourceOptions().withCachePath(path), ClientOptions());
        StillScene scene(loader);
        const Still still = scene.render();
        ASSERT_TRUE(still.finished) << "an offline still frame waited on a missing DEM tile for more than "
                                    << stillLimit.count() << " s";
        ASSERT_FALSE(still.error);
        EXPECT_FALSE(scene.elevation(center)) << "a DEM came from somewhere";
        const double off = differingFraction(still.image, flat);
        RecordProperty("differsFromFlatPermille", static_cast<int>(off * 1000));
        EXPECT_LE(off, 0.01) << "the offline frame is not the flat frame at 0 m";
    }
    deleteDatabase(path);
}
