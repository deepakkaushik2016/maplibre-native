#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_observer.hpp>
#include <mln/map/map_options.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/storage/main_resource_loader.hpp>
#include <mln/storage/network_status.hpp>
#include <mln/storage/offline_database.hpp>
#include <mln/storage/resource.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/storage/response.hpp>
#include <mln/style/style.hpp>
#include <mln/map/transform_state.hpp>
#include <mln/storage/cache_generation.hpp>
#include <mln/tile/raster_dem_tile.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/util/chrono.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/http_header.hpp>
#include <mln/util/image.hpp>
#include <mln/util/io.hpp>
#include <mln/util/mat4.hpp>
#include <mln/util/projection.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/tile_server_options.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace mln;

// A sparse raster-dem tileset, as the production terrain tileset is: every tile to z12, z13-14
// only inside some boxes, and an empty answer (204 No Content, or a 404) for every tile it does not
// have. A tile that answers empty at or below the source's maxzoom means "no finer data here": the
// map draws the terrain, and answers elevation queries, from the nearest ancestor that has data,
// overzoomed - as maplibre-gl-js falls back to a loaded parent for a tile that failed or is empty.
//
// The ground is a cone, so a frame drawn flat (or an elevation of 0 m, or none) is easy to tell
// from the cone overzoomed from z12. Each scene reaches z13.5 outside the boxes in the ways a user
// does: zooming in from z12, where the z12 tiles are loaded already (and from z13.2, within the
// level), and a cold jump straight to z13.5 (a new map, no frame before), where nothing is.

namespace {

constexpr uint32_t viewWidth = 400;
constexpr uint32_t viewHeight = 600;
constexpr int planetMaxZoom = 12;
constexpr int demMaxZoom = 14;
constexpr uint32_t demSize = 512;
constexpr double kmPerDegree = 6371.0088 * M_PI / 180.0;

// The z12 tile the cone stands on: its apex is the tile's centre (about 46.6 N, 10.3 E).
constexpr uint32_t apexX = 2165;
constexpr uint32_t apexY = 1447;
constexpr double coneBase = 400.0;
constexpr double conePeak = 2400.0;
constexpr double coneRadiusKm = 15.0;

LatLng tilePoint(double x, double y, int z = planetMaxZoom) {
    const double n = std::pow(2.0, z);
    return {util::rad2deg(std::atan(std::sinh(M_PI * (1.0 - 2.0 * y / n)))), x / n * 360.0 - 180.0};
}

// Where `point` is in z12 tile units.
std::pair<double, double> tileCoordinates(const LatLng& point) {
    const double n = std::pow(2.0, planetMaxZoom);
    const double lat = util::deg2rad(point.latitude());
    return {(point.longitude() + 180.0) / 360.0 * n,
            (1.0 - std::log(std::tan(lat) + 1.0 / std::cos(lat)) / M_PI) / 2.0 * n};
}

const LatLng apex = tilePoint(apexX + 0.5, apexY + 0.5);

double coneMeters(const LatLng& point) {
    const double northKm = (point.latitude() - apex.latitude()) * kmPerDegree;
    const double eastKm = (point.longitude() - apex.longitude()) * kmPerDegree *
                          std::cos(util::deg2rad(apex.latitude()));
    const double km = std::hypot(northKm, eastKm);
    return coneBase + (conePeak - coneBase) * std::max(0.0, 1.0 - km / coneRadiusKm);
}

// The box with z13-14 tiles: the z12 tile two east and two south of the apex's, out of every view
// below that does not look into it. Its z13-14 tiles hold a bump the z12 tile does not: the finer
// detail the production tileset has inside a park.
const CanonicalTileID subBox(planetMaxZoom, apexX + 2, apexY + 2);
const LatLng bump = tilePoint(subBox.x + 0.5, subBox.y + 0.5);
constexpr double bumpMeters = 120.0;
constexpr double bumpWidthKm = 0.25;

double bumpAt(const LatLng& point) {
    const double northKm = (point.latitude() - bump.latitude()) * kmPerDegree;
    const double eastKm = (point.longitude() - bump.longitude()) * kmPerDegree *
                          std::cos(util::deg2rad(bump.latitude()));
    return bumpMeters * std::exp(-(northKm * northKm + eastKm * eastKm) / (bumpWidthKm * bumpWidthKm));
}

// The cut-out (Japan, the Northern Territories, Takeshima in the production tileset): the column
// of two z12 tiles south-east of the apex's, (apexX + 1, apexY + 1) and (apexX + 1, apexY + 2), at
// 0 m. They are the west half of z11 tile (1083, 724), whose east half is real ground, so that
// tile (and every one above it) straddles the coast and holds 0 m pixels on the cut's side; the z12
// tiles inside, and every tile under them, answer empty.
bool inCut(const CanonicalTileID& id) {
    if (id.z < planetMaxZoom) {
        return false; // a z11 tile spans two z12 columns: it straddles at most
    }
    const uint32_t x = id.x >> (id.z - planetMaxZoom);
    const uint32_t y = id.y >> (id.z - planetMaxZoom);
    return x == apexX + 1 && (y == apexY + 1 || y == apexY + 2);
}

bool inCut(const LatLng& point) {
    const auto [x, y] = tileCoordinates(point);
    return std::floor(x) == apexX + 1 && (std::floor(y) == apexY + 1 || std::floor(y) == apexY + 2);
}

// The coast: the cut's west edge, at x = apexX + 1 in z12 tile units.
const double coastX = apexX + 1.0;

enum class Ground {
    Cone,
    ConeWithCut,
    RaisedCone, // the cone 50 m higher: data a tile gains later (an offline pack, say)
    HighCone,   // the cone 4500 m higher: ground above the eye of a camera whose centre is at 0 m
    HighSlope,  // HighCone rising eastwards, so that places apart have different heights
};
constexpr double raisedBy = 50.0;
constexpr double highConeBy = 4500.0;

double groundMeters(Ground ground, const LatLng& point, bool withBump) {
    if (ground == Ground::ConeWithCut && inCut(point)) {
        return 0.0;
    }
    return coneMeters(point) + (withBump ? bumpAt(point) : 0.0) + (ground == Ground::RaisedCone ? raisedBy : 0.0) +
           (ground == Ground::HighCone || ground == Ground::HighSlope ? highConeBy : 0.0) +
           (ground == Ground::HighSlope ? (point.longitude() - apex.longitude()) * 2000.0 : 0.0);
}

// The DEM tile z/x/y of the ground in Terrarium, each pixel holding the height at its centre.
// Memoized: a 512 px tile takes a while to encode, and the scenes ask for the same ones.
std::shared_ptr<const std::string> demTile(Ground ground, const CanonicalTileID& id) {
    static std::map<std::pair<int, CanonicalTileID>, std::shared_ptr<const std::string>> tiles;
    auto& tile = tiles[{static_cast<int>(ground), id}];
    if (tile) {
        return tile;
    }
    const bool withBump = id.z > planetMaxZoom;
    const double n = std::pow(2.0, id.z);
    PremultipliedImage image({demSize, demSize});
    for (uint32_t row = 0; row < demSize; ++row) {
        const double mercatorY = (id.y + (row + 0.5) / demSize) / n;
        const double latitude = util::rad2deg(std::atan(std::sinh(M_PI * (1.0 - 2.0 * mercatorY))));
        for (uint32_t col = 0; col < demSize; ++col) {
            const double longitude = (id.x + (col + 0.5) / demSize) / n * 360.0 - 180.0;
            const double value = groundMeters(ground, {latitude, longitude}, withBump) + 32768.0;
            const auto whole = static_cast<uint32_t>(std::floor(value));
            uint8_t* px = image.data.get() + (static_cast<size_t>(row) * demSize + col) * 4;
            px[0] = static_cast<uint8_t>(whole >> 8);
            px[1] = static_cast<uint8_t>(whole & 0xFF);
            px[2] = static_cast<uint8_t>(std::floor((value - whole) * 256.0));
            px[3] = 255;
        }
    }
    tile = std::make_shared<const std::string>(encodePNG(image));
    return tile;
}

// How the tileset answers a tile it does not have.
enum class Missing {
    NoContent, // 204 with an empty body, as the production tileset answers
    NotFound,  // 404
};

// Which tiles the tileset has.
enum class TilesetKind {
    Sparse, // z0-12 everywhere, z13-14 in the sub-box only (and nothing inside the cut)
    Dense,  // every tile to z14: the reference a sparse tileset is drawn against
};

// Lines every 250 m around the apex, draped on the terrain, so that the drawn mesh's height shows:
// a mesh drawn at the wrong height moves the lines on the screen.
std::string gridGeoJSON() {
    const double kmPerLng = kmPerDegree * std::cos(util::deg2rad(apex.latitude()));
    std::string lines;
    for (int i = -60; i <= 60; ++i) {
        const double km = i * 0.25;
        const double lat = apex.latitude() + km / kmPerDegree;
        const double lng = apex.longitude() + km / kmPerLng;
        const double south = apex.latitude() - 15.0 / kmPerDegree;
        const double north = apex.latitude() + 15.0 / kmPerDegree;
        const double west = apex.longitude() - 15.0 / kmPerLng;
        const double east = apex.longitude() + 15.0 / kmPerLng;
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

// A marker on the cone's flank, in the view of every flank scene below: a red circle, drawn at the
// height of the ground under it (SceneOptions::marker).
const LatLng marker = tilePoint(apexX - 0.5, apexY + 1.45);

// The production source's shape: one raster-dem source `terrain`, Terrarium, 512 px, maxzoom 14.
// A colour relief and a hillshade of it, and the grid, all draped. `terrain` false leaves the
// terrain out: the same layers, drawn flat. `withMarker` adds the marker. `hillshadeOnly` draws
// the hillshade alone over the background, as the device's stills did.
std::string sparseStyle(bool terrain = true,
                        const std::string& tiles = "terrain",
                        bool withMarker = false,
                        bool hillshadeOnly = false) {
    const std::string markerSource =
        withMarker ? R"(, "marker": {"type": "geojson", "data": {"type": "Point", "coordinates": [)" +
                         std::to_string(marker.longitude()) + "," + std::to_string(marker.latitude()) + "]}}"
                   : "";
    const std::string markerLayer =
        withMarker
            ? R"(, {"id": "marker", "type": "circle", "source": "marker", "paint": {"circle-color": "#f00", "circle-radius": 8, "circle-pitch-alignment": "viewport"}})"
            : "";
    return R"STYLE({
  "version": 8,
  "sources": {
    "terrain": {
      "type": "raster-dem",
      "tiles": ["http://example.com/)STYLE" +
           tiles + R"STYLE(/{z}/{x}/{y}.png"],
      "encoding": "terrarium",
      "maxzoom": )STYLE" +
           std::to_string(demMaxZoom) + R"STYLE(,
      "tileSize": 512
    },
    "grid": {"type": "geojson", "data": )STYLE" +
           gridGeoJSON() + "}" + markerSource + R"STYLE(
  },)STYLE" +
           (terrain ? R"STYLE(
  "terrain": {"source": "terrain", "exaggeration": 1},)STYLE"
                    : "") +
           R"STYLE(
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "#dddddd"}},
    )STYLE" +
           (hillshadeOnly ? std::string()
                          : std::string(R"STYLE({"id": "relief", "type": "color-relief", "source": "terrain",
     "paint": {"color-relief-color": ["interpolate", ["linear"], ["elevation"],
        0, "rgb(20, 60, 160)", 400, "rgb(20, 120, 40)", 2400, "rgb(150, 90, 30)", 4900, "rgb(20, 60, 160)", 6900, "rgb(150, 90, 30)"]}},
    )STYLE")) +
           R"STYLE({"id": "hillshade", "type": "hillshade", "source": "terrain", "paint": {"hillshade-exaggeration": 0.4}})STYLE" +
           (hillshadeOnly ? std::string() : std::string(R"STYLE(,
    {"id": "grid", "type": "line", "source": "grid", "paint": {"line-color": "black", "line-width": 2}})STYLE")) +
           R"STYLE(
  )STYLE" + markerLayer +
           R"STYLE(]
})STYLE";
}

// A file source that, like DefaultFileSource, answers each tile from its (here empty) cache first
// and from the network after.
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
    void onCameraDidChange(CameraChangeMode mode) override {
        if (mode == CameraChangeMode::Animated) {
            ++transitionsEnded;
        }
    }
    int frames = 0;
    std::set<std::pair<OverscaledTileID, std::string>> parsing;
    bool idleFrame = false;
    int transitionsEnded = 0;
};

struct SceneOptions {
    TilesetKind tileset = TilesetKind::Sparse;
    Missing missing = Missing::NoContent;
    Ground ground = Ground::Cone;
    bool terrain = true;
    /// The Cache-Control header of an empty answer, which sets its expiry as the HTTP file sources do.
    std::optional<std::string> emptyCacheControl;
    /// Whether the style has the marker.
    bool marker = false;
    /// The map's mode: a static map draws only when asked to, and its tiles load for the still.
    MapMode mode = MapMode::Continuous;
    /// The hillshade alone over the background (sparseStyle).
    bool hillshadeOnly = false;
    uint32_t width = viewWidth;
    uint32_t height = viewHeight;
};

// A map run continuously, so tiles arrive across frames as they do in an app. Nothing is drawn
// until the first camera is set: a new scene is a cold start.
struct SparseScene {
    util::RunLoop loop;
    SceneOptions options;
    std::shared_ptr<CachingFileSource> fileSource = std::make_shared<CachingFileSource>();
    HeadlessFrontend frontend{{options.width, options.height}, 1};
    FrameObserver observer;
    MapAdapter map{frontend, observer, fileSource, MapOptions().withMapMode(options.mode).withSize(frontend.getSize())};
    /// Network requests per DEM tile.
    std::map<CanonicalTileID, int> networkRequests;
    /// Cache-only requests per DEM tile.
    std::map<CanonicalTileID, int> cacheRequests;
    /// Tiles the cache answers with data, and with which.
    std::map<CanonicalTileID, Ground> cached;
    /// Tiles the network answers with data though the tileset had none, and with which.
    std::map<CanonicalTileID, Ground> gained;

    explicit SparseScene(SceneOptions options_ = {})
        : options(options_) {
        fileSource->tileResponse = [this](const Resource& resource) -> std::optional<Response> {
            Response res;
            if (!resource.tileData || resource.url.find("/terrain") == std::string::npos) {
                res.noContent = true;
                return res;
            }
            const CanonicalTileID id(
                resource.tileData->z, static_cast<uint32_t>(resource.tileData->x), resource.tileData->y);
            if (resource.loadingMethod == Resource::LoadingMethod::CacheOnly) {
                ++cacheRequests[id];
                if (const auto it = cached.find(id); it != cached.end()) {
                    res.data = demTile(it->second, id);
                    return res;
                }
                res.error = std::make_unique<Response::Error>(Response::Error::Reason::NotFound, "not cached");
                return res;
            }
            ++networkRequests[id];
            if (resource.url.find("/terrain-v2/") != std::string::npos) {
                // A new version of the tileset, with every tile, 50 m higher.
                res.data = demTile(Ground::RaisedCone, id);
                return res;
            }
            if (const auto it = gained.find(id); it != gained.end()) {
                res.data = demTile(it->second, id);
                return res;
            }
            if (!has(id)) {
                if (options.missing == Missing::NoContent) {
                    res.noContent = true;
                } else {
                    res.error = std::make_unique<Response::Error>(Response::Error::Reason::NotFound, "no tile");
                }
                if (options.emptyCacheControl) {
                    res.expires = http::CacheControl::parse(*options.emptyCacheControl).toTimePoint();
                }
                return res;
            }
            res.data = demTile(options.ground, id);
            return res;
        };
        map.getStyle().loadJSON(sparseStyle(options.terrain, "terrain", options.marker, options.hillshadeOnly));
    }

    bool has(const CanonicalTileID& id) const {
        if (options.tileset == TilesetKind::Dense) {
            return true; // the cut too, its tiles at 0 m
        }
        if (options.ground == Ground::ConeWithCut && inCut(id)) {
            return false;
        }
        if (id.z <= planetMaxZoom) {
            return true;
        }
        return CanonicalTileID(planetMaxZoom, id.x >> (id.z - planetMaxZoom), id.y >> (id.z - planetMaxZoom)) == subBox;
    }

    double ground(const LatLng& point) const {
        // Only the sub-box's z13-14 tiles hold the bump, and in a dense tileset every tile there
        // does; the scenes below look at the bump only where it is drawn from those.
        return groundMeters(options.ground, point, false) + bumpAt(point);
    }

    CameraOptions camera(const LatLng& center, double zoom, double pitch) const {
        return CameraOptions().withCenter(center).withZoom(zoom).withPitch(pitch).withBearing(0.0).withCenterAltitude(
            options.terrain ? ground(center) : 0.0);
    }

    void jumpTo(const CameraOptions& camera) {
        map.jumpTo(camera);
        settle();
    }

    // Zooms there as a user does, a frame at a time over a second, and waits for the tiles.
    void easeTo(const CameraOptions& camera) {
        const int ended = observer.transitionsEnded;
        map.easeTo(camera, AnimationOptions(Milliseconds(1000)));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (observer.transitionsEnded == ended && std::chrono::steady_clock::now() < deadline) {
            loop.runOnce();
        }
        EXPECT_NE(observer.transitionsEnded, ended) << "the zoom did not end";
        settle();
    }

    // Runs the map until every tile that can arrive has, has been parsed and drawn, and a frame
    // has found nothing more to draw: at most 30 s, failing the test past that.
    void settle() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        const int start = observer.frames;
        observer.idleFrame = false;
        const auto quiet = [&] {
            return observer.frames > start && observer.parsing.empty() && observer.idleFrame;
        };
        while (std::chrono::steady_clock::now() < deadline) {
            loop.runOnce();
            if (quiet()) {
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

    PremultipliedImage image() {
        gfx::BackendScope guard{*frontend.getBackend()};
        return frontend.readStillImage();
    }

    std::optional<double> elevation(const LatLng& point) {
        return frontend.getRenderer()->queryTerrainElevation(point);
    }

    int requestsAbove(int zoom) const {
        int count = 0;
        for (const auto& [id, n] : networkRequests) {
            if (id.z > zoom) {
                count += n;
            }
        }
        return count;
    }
};

// The share of pixels that differ from `reference` by more than 8 levels in a channel, leaving out
// the columns within `bandHalfWidth` pixels of the screen's vertical centre line.
double mismatch(const PremultipliedImage& image, const PremultipliedImage& reference, double bandHalfWidth = -1.0) {
    if (image.size != reference.size) {
        return 1.0;
    }
    size_t count = 0;
    size_t total = 0;
    const double middle = image.size.width / 2.0;
    for (uint32_t row = 0; row < image.size.height; ++row) {
        for (uint32_t col = 0; col < image.size.width; ++col) {
            if (std::abs(col + 0.5 - middle) <= bandHalfWidth) {
                continue;
            }
            ++total;
            const size_t i = (static_cast<size_t>(row) * image.size.width + col) * 4;
            for (size_t c = 0; c < 4; ++c) {
                if (std::abs(image.data[i + c] - reference.data[i + c]) > 8) {
                    ++count;
                    break;
                }
            }
        }
    }
    return total ? static_cast<double>(count) / static_cast<double>(total) : 1.0;
}

// The view: the centre of the z12 tile south-west of the apex's, about 9.5 km from the apex, on
// the cone's flank at about 1130 m, looking north up the slope. At z13.5 it sees z12 tile
// (apexX - 1, apexY + 1) and the one north of it, none of the sub-box or the cut.
const LatLng flank = tilePoint(apexX - 0.5, apexY + 1.5);
constexpr double arrivalZoom = 13.5;
constexpr double pitch = 50.0;

// Points round the view's centre, on the flank: the ground the view looks at.
std::vector<LatLng> flankPoints() {
    const double kmPerLng = kmPerDegree * std::cos(util::deg2rad(flank.latitude()));
    std::vector<LatLng> points;
    for (const double northKm : {-0.4, 0.0, 0.5, 1.0}) {
        for (const double eastKm : {-0.5, 0.0, 0.5}) {
            points.emplace_back(flank.latitude() + northKm / kmPerDegree, flank.longitude() + eastKm / kmPerLng);
        }
    }
    return points;
}

// The most the drawn frame may differ from the dense reference's, which draws the cone from z14
// DEM tiles where the sparse scene overzooms its z12 ones (3.3 m against 13 m a DEM pixel). The
// surface is the same (the cone is linear in distance: bilinear interpolation of it misses by under
// a millimetre at either size); the colour relief and hillshade sample textures of different
// resolutions, within the 8 levels a channel may differ by, and the grid's anti-aliased edges may
// fall a fraction of a pixel apart. Measured: 0 on Metal, at z13.5 and z15.5. A frame drawn flat
// moves the grid: 18 % of the frame at z13.5, 4.5 % at z15.5, where fewer grid lines are in view;
// each scene checks first that the flat frame differs from the reference by at least 4 times the
// bound. A drape baked at another zoom draws the grid's lines 2^0.3 too wide: 8-11 % at z13.5.
constexpr double maxMismatch = 0.01;
constexpr double minFlatMismatch = 0.04;
// The cut drawn from the z11 tile against z12-14 tiles at 0 m: the frames differ only where the
// coarser DEM draws the coast itself. On the cut's side the z11 surface runs down the cliff over a
// z11 DEM pixel (26 m) instead of a z14 one; on the land's side the z12 tile's border, with no
// neighbour to backfill it, repeats its edge pixels, so the hillshade there does not see the cliff.
// The band left out is a z11 DEM pixel and a half either side of the coast, and 2 px of
// anti-aliasing; outside it the frames match (measured: 0 on Metal).
constexpr double maxCoastMismatch = 0.002;
double coastBand(double zoom) {
    return 1.5 * std::exp2(zoom - (planetMaxZoom - 1)) + 2.0;
}

// The elevation query reads the cone at every point: overzoomed from z12, the bilinear surface is
// the cone to well under a millimetre, and Terrarium holds heights to 1/256 m.
void expectTheCone(SparseScene& scene, const std::string& when) {
    for (const auto& point : flankPoints()) {
        const auto meters = scene.elevation(point);
        ASSERT_TRUE(meters) << when << ": no elevation at " << point.latitude() << ", " << point.longitude();
        EXPECT_NEAR(*meters, coneMeters(point), 0.25)
            << when << ": at " << point.latitude() << ", " << point.longitude();
    }
    // The ground under the middle of the screen, raycast from the camera: where the camera looks.
    const auto ground = scene.frontend.getRenderer()->queryTerrainLatLng({viewWidth / 2.0, viewHeight / 2.0});
    ASSERT_TRUE(ground) << when << ": no ground under the screen's centre";
    EXPECT_NEAR(ground->latitude(), flank.latitude(), 0.0005) << when;
    EXPECT_NEAR(ground->longitude(), flank.longitude(), 0.0005) << when;
}

PremultipliedImage reference(double zoom, const LatLng& center, SceneOptions options) {
    options.tileset = TilesetKind::Dense;
    SparseScene scene(options);
    scene.jumpTo(scene.camera(center, zoom, pitch));
    return scene.image();
}

void expectDrawnAsTheCone(SparseScene& scene, const std::string& when, double zoom = arrivalZoom) {
    // The dense and the flat frame at each zoom, drawn once.
    static std::map<double, std::pair<PremultipliedImage, PremultipliedImage>> references;
    if (!references.contains(zoom)) {
        SceneOptions flatOptions;
        flatOptions.terrain = false;
        references.emplace(zoom, std::pair{reference(zoom, flank, {}), reference(zoom, flank, flatOptions)});
    }
    const auto& [dense, flat] = references.at(zoom);
    ASSERT_GE(mismatch(flat, dense), minFlatMismatch) << "a flat frame looks like the cone: the test proves nothing";
    const double share = mismatch(scene.image(), dense);
    EXPECT_LE(share, maxMismatch) << when << ": " << share * 100.0 << " % of the frame differs from the cone ("
                                  << mismatch(scene.image(), flat) * 100.0 << " % from the flat frame)";
}

// A still image of the flank (the tests are below), at the size and camera of
// the device's stills, waited for at most stillLimit so that one that never finishes fails.
constexpr auto stillLimit = std::chrono::seconds(20);
constexpr uint32_t stillSize = 256;

struct Still {
    bool finished = false;
    std::exception_ptr error;
    PremultipliedImage image;
};

Still renderStillWithin(HeadlessFrontend& frontend, Map& map) {
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
    const auto deadline = std::chrono::steady_clock::now() + stillLimit;
    while (!done && std::chrono::steady_clock::now() < deadline) {
        util::RunLoop::Get()->runOnce();
    }
    still.finished = done;
    return still;
}

SceneOptions stillOptions(Ground ground, Missing missing, TilesetKind tileset, bool hillshadeOnly) {
    SceneOptions options;
    options.ground = ground;
    options.missing = missing;
    options.tileset = tileset;
    options.hillshadeOnly = hillshadeOnly;
    options.mode = MapMode::Static;
    options.width = stillSize;
    options.height = stillSize;
    return options;
}

// The share of the frame that is the background (#ddd).
double backgroundShare(const PremultipliedImage& image) {
    size_t count = 0;
    for (size_t i = 0; i < image.bytes(); i += 4) {
        const uint8_t* px = image.data.get() + i;
        count += std::abs(px[0] - 0xdd) <= 4 && std::abs(px[1] - 0xdd) <= 4 && std::abs(px[2] - 0xdd) <= 4 ? 1 : 0;
    }
    return static_cast<double>(count) / (static_cast<double>(image.bytes()) / 4.0);
}

// A static map whose camera says nothing of the height of its centre, as a share image's says, is
// drawn with the centre on the ground under it when the clamp is on: the same frame as a dense
// tileset's with the height of the centre given.
void expectStillOnTheGround(Ground ground, Missing missing, bool hillshadeOnly) {
    PremultipliedImage reference;
    {
        SparseScene dense(stillOptions(ground, missing, TilesetKind::Dense, hillshadeOnly));
        dense.map.jumpTo(CameraOptions()
                             .withCenter(flank)
                             .withZoom(arrivalZoom)
                             .withPitch(pitch)
                             .withBearing(0.0)
                             .withCenterAltitude(groundMeters(ground, flank, false)));
        Still still = renderStillWithin(dense.frontend, dense.map);
        ASSERT_TRUE(still.finished);
        ASSERT_FALSE(still.error);
        ASSERT_LT(backgroundShare(still.image), 0.5) << "the reference shows no ground: the test proves nothing";
        reference = std::move(still.image);
    }
    SparseScene scene(stillOptions(ground, missing, TilesetKind::Sparse, hillshadeOnly));
    scene.map.setCenterClampedToGround(true);
    scene.map.jumpTo(CameraOptions().withCenter(flank).withZoom(arrivalZoom).withPitch(pitch).withBearing(0.0));
    const Still still = renderStillWithin(scene.frontend, scene.map);
    ASSERT_TRUE(still.finished) << "the still image never finished";
    ASSERT_FALSE(still.error);
    ASSERT_GT(scene.requestsAbove(planetMaxZoom), 0) << "the map never asked for a z13-14 tile";
    EXPECT_NEAR(*scene.map.getCameraOptions().centerAltitude, groundMeters(ground, flank, false), 1.0);
    EXPECT_LT(backgroundShare(still.image), 0.5) << "only the background was drawn";
    const double share = mismatch(still.image, reference);
    EXPECT_LE(share, maxMismatch) << share * 100.0 << " % of the frame differs from the dense reference";
}

// (A) zoom in from z12, where the z12 tiles are loaded, to z13.5 outside the sub-box.
void expectZoomInKeepsTheShape(Missing missing, double zoom = arrivalZoom) {
    SceneOptions options;
    options.missing = missing;
    SparseScene scene(options);
    scene.jumpTo(scene.camera(flank, planetMaxZoom, pitch));
    expectTheCone(scene, "at z12");
    scene.easeTo(scene.camera(flank, zoom, pitch));
    ASSERT_GT(scene.requestsAbove(planetMaxZoom), 0) << "the map never asked for a z13-14 tile";
    const std::string when = "zoomed in to z" + std::to_string(zoom);
    expectTheCone(scene, when);
    expectDrawnAsTheCone(scene, when, zoom);
}

// (A) within a zoom level: from z13.2, where the mesh is on z13 tiles, to z13.5, where it is on z14
// ones. The integer zoom and every draped tile stay the same, and so does the DEM, all overzoomed
// from z12: only the drape targets are new.
void expectZoomWithinALevelKeepsTheShape(Missing missing) {
    SceneOptions options;
    options.missing = missing;
    SparseScene scene(options);
    scene.jumpTo(scene.camera(flank, 13.2, pitch));
    expectTheCone(scene, "at z13.2");
    scene.jumpTo(scene.camera(flank, arrivalZoom, pitch));
    expectTheCone(scene, "zoomed in to z13.5");
    expectDrawnAsTheCone(scene, "zoomed in to z13.5");
}

// (B) a cold jump: a new map straight at z13.5, no frame drawn before.
void expectColdJumpKeepsTheShape(Missing missing, double zoom = arrivalZoom) {
    SceneOptions options;
    options.missing = missing;
    SparseScene scene(options);
    scene.jumpTo(scene.camera(flank, zoom, pitch));
    ASSERT_GT(scene.requestsAbove(planetMaxZoom), 0) << "the map never asked for a z13-14 tile";
    const std::string when = "jumped to z" + std::to_string(zoom);
    expectTheCone(scene, when);
    expectDrawnAsTheCone(scene, when, zoom);
}

} // namespace

// A still image (Task 38, device defect D2). The device's stills over the Himalaya drew only the
// background: nothing of the sparse DEM was missing, the camera was under the ground. The DEM
// heights came in with the first frame, too late for the clamp, which moved the centre only when
// that frame was already the still; and a static map has no clamp unless it is asked for.
TEST(TerrainSparseDEM, AStillDrawsHighGroundWithTheCenterOnIt) {
    expectStillOnTheGround(Ground::HighCone, Missing::NotFound, /*hillshadeOnly=*/true);
}

TEST(TerrainSparseDEM, AStillDrawsHighGroundWithTheCenterOnItOn204) {
    expectStillOnTheGround(Ground::HighCone, Missing::NoContent, /*hillshadeOnly=*/false);
}

TEST(TerrainSparseDEM, AStillDrawsLowGroundWithTheCenterOnIt) {
    expectStillOnTheGround(Ground::Cone, Missing::NotFound, /*hillshadeOnly=*/false);
}

// The clamp is the map's option, off unless asked for (Map::setCenterClampedToGround): a still
// without it keeps the camera as it was set, and over high ground that is under the ground, as it
// is on a continuous map. A share image has to ask for the clamp, or give the centre's height.
TEST(TerrainSparseDEM, AStillWithoutTheClampKeepsTheCentreWhereItWasSet) {
    SparseScene scene(stillOptions(Ground::HighCone, Missing::NotFound, TilesetKind::Sparse, /*hillshadeOnly=*/true));
    scene.map.jumpTo(CameraOptions().withCenter(flank).withZoom(arrivalZoom).withPitch(pitch).withBearing(0.0));
    const Still still = renderStillWithin(scene.frontend, scene.map);
    ASSERT_TRUE(still.finished);
    ASSERT_FALSE(still.error);
    EXPECT_EQ(*scene.map.getCameraOptions().centerAltitude, 0.0);
    // The accepted blank: a default that lifts such a camera has to change this test on purpose.
    EXPECT_GT(backgroundShare(still.image), 0.99);
}

// An anchored zoom owes a still on a reused map an anchor that slides the centre along the line of
// sight, in passes (Map::renderStill(camera) does not settle it): each pass moves the camera, so
// the still has to wait for its frame. It finishes with the centre on the ground under it; a still
// that finished on the frame before a pass would leave it the pass's residual off the ground.
TEST(TerrainSparseDEM, AStillOfAReusedMapWaitsForTheAnchorsPasses) {
    SparseScene scene(stillOptions(Ground::HighCone, Missing::NotFound, TilesetKind::Sparse, /*hillshadeOnly=*/true));
    scene.map.setCenterClampedToGround(true);
    scene.map.jumpTo(CameraOptions().withCenter(flank).withZoom(arrivalZoom).withPitch(pitch).withBearing(0.0));
    ASSERT_TRUE(renderStillWithin(scene.frontend, scene.map).finished);

    scene.map.jumpTo(CameraOptions().withZoom(arrivalZoom + 0.5).withAnchor(ScreenCoordinate{128.0, 60.0}));
    Still still;
    bool done = false;
    {
        gfx::BackendScope guard{*scene.frontend.getBackend()};
        scene.map.renderStill(scene.map.getCameraOptions(), MapDebugOptions::NoDebug, [&](const std::exception_ptr& e) {
            still.error = e;
            if (!e) {
                still.image = scene.frontend.readStillImage();
            }
            done = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + stillLimit;
        while (!done && std::chrono::steady_clock::now() < deadline) {
            util::RunLoop::Get()->runOnce();
        }
    }
    ASSERT_TRUE(done) << "the still image never finished";
    ASSERT_FALSE(still.error);
    const CameraOptions camera = scene.map.getCameraOptions();
    EXPECT_NEAR(*camera.centerAltitude, groundMeters(Ground::HighCone, *camera.center, false), 1.0)
        << "the still finished before the anchor's passes put the centre on the ground";
    EXPECT_LT(backgroundShare(still.image), 0.5);
}

// A still that fails after drawing again for the clamp leaves nothing to the next one: eight of
// them in a row (the cap) would use up the redraws of a still that kept them, which would then draw
// from under the ground.
TEST(TerrainSparseDEM, AStillThatFailedLeavesTheNextItsRedraws) {
    // The colour relief: a hillshade of ground this gentle is close to the background.
    SparseScene scene(stillOptions(Ground::HighSlope, Missing::NotFound, TilesetKind::Sparse, /*hillshadeOnly=*/false));
    scene.map.setCenterClampedToGround(true);
    // Fails each tile first asked for after the clamp has moved the camera: the frame drawn again
    // for the clamp needs tiles the frame before it did not.
    std::set<CanonicalTileID> asked;
    bool failNewTiles = true;
    const auto tiles = scene.fileSource->tileResponse;
    scene.fileSource->tileResponse = [&](const Resource& resource) -> std::optional<Response> {
        if (resource.tileData && resource.loadingMethod != Resource::LoadingMethod::CacheOnly) {
            const CanonicalTileID id(
                resource.tileData->z, static_cast<uint32_t>(resource.tileData->x), resource.tileData->y);
            const bool isNew = asked.insert(id).second;
            if (failNewTiles && isNew && scene.map.getCameraOptions().centerAltitude.value_or(0.0) > 1.0) {
                Response res;
                res.error = std::make_unique<Response::Error>(Response::Error::Reason::Server,
                                                              "no tile for this still");
                return res;
            }
        }
        return tiles(resource);
    };
    const auto cameraAt = [&](int place) {
        // Far enough apart that no tile of one place serves another.
        return CameraOptions()
            .withCenter(LatLng(flank.latitude(), flank.longitude() + 0.4 * place))
            .withZoom(arrivalZoom)
            .withPitch(pitch)
            .withBearing(0.0)
            .withCenterAltitude(0.0);
    };
    // A still that finds no new tile finishes, and a finished still resets the count: eight in a
    // row, as the cap needs.
    int inARow = 0;
    int place = 0;
    for (; place < 40 && inARow < 8; ++place) {
        scene.map.jumpTo(cameraAt(place));
        const Still still = renderStillWithin(scene.frontend, scene.map);
        ASSERT_TRUE(still.finished);
        inARow = still.error ? inARow + 1 : 0;
    }
    ASSERT_EQ(inARow, 8) << "the stills did not fail after drawing again: the test proves nothing";

    failNewTiles = false;
    scene.map.jumpTo(cameraAt(place));
    const Still still = renderStillWithin(scene.frontend, scene.map);
    ASSERT_TRUE(still.finished);
    ASSERT_FALSE(still.error);
    EXPECT_LT(backgroundShare(still.image), 0.5) << "the still drew from under the ground";
}

namespace {

// An offline database at `path` holding the DEM tiles of the view, z0-12 only, over high ground.
class OfflineDEM {
public:
    explicit OfflineDEM(std::string path_)
        : path(std::move(path_)) {
        remove();
    }
    ~OfflineDEM() { remove(); }
    OfflineDEM(const OfflineDEM&) = delete;
    OfflineDEM& operator=(const OfflineDEM&) = delete;

    void putView() {
        OfflineDatabase db(path, TileServerOptions::DefaultConfiguration());
        std::set<CanonicalTileID> ancestors;
        for (int64_t x = int64_t{apexX} - 3; x <= int64_t{apexX} + 3; ++x) {
            for (int64_t y = int64_t{apexY} - 2; y <= int64_t{apexY} + 3; ++y) {
                for (uint8_t z = planetMaxZoom; z > 0; --z) {
                    const int dz = planetMaxZoom - z;
                    const CanonicalTileID id(z, static_cast<uint32_t>(x) >> dz, static_cast<uint32_t>(y) >> dz);
                    if (ancestors.insert(id).second) {
                        Response response;
                        response.data = demTile(Ground::HighCone, id);
                        db.put(Resource::tile("http://example.com/terrain/{z}/{x}/{y}.png",
                                              1,
                                              id.x,
                                              id.y,
                                              id.z,
                                              Tileset::Scheme::XYZ),
                               response);
                    }
                }
            }
        }
    }

    const std::string path;

private:
    void remove() {
        util::deleteFile(path);
        util::deleteFile(path + "-wal");
        util::deleteFile(path + "-journal");
        util::deleteFile(path + "-shm");
    }
};

struct OfflineNetwork {
    OfflineNetwork() { NetworkStatus::Set(NetworkStatus::Status::Offline); }
    ~OfflineNetwork() { NetworkStatus::Set(NetworkStatus::Status::Online); }
    OfflineNetwork(const OfflineNetwork&) = delete;
    OfflineNetwork& operator=(const OfflineNetwork&) = delete;
};

} // namespace

// Offline, the z12 ancestors in the database are drawn for the z13-14 tiles it does not have, with
// the centre on them, as online.
TEST(TerrainSparseDEM, AnOfflineStillDrawsTheAncestorInTheDatabase) {
    PremultipliedImage reference;
    {
        SparseScene dense(
            stillOptions(Ground::HighCone, Missing::NotFound, TilesetKind::Dense, /*hillshadeOnly=*/true));
        dense.map.jumpTo(CameraOptions()
                             .withCenter(flank)
                             .withZoom(arrivalZoom)
                             .withPitch(pitch)
                             .withBearing(0.0)
                             .withCenterAltitude(groundMeters(Ground::HighCone, flank, false)));
        Still still = renderStillWithin(dense.frontend, dense.map);
        ASSERT_TRUE(still.finished);
        ASSERT_FALSE(still.error);
        reference = std::move(still.image);
    }
    util::RunLoop loop;
    OfflineDEM database("test/fixtures/offline_database/terrain_sparse_still.db");
    database.putView();
    OfflineNetwork offline;
    auto fileSource = std::make_shared<MainResourceLoader>(ResourceOptions().withCachePath(database.path),
                                                           ClientOptions());
    FrameObserver observer;
    HeadlessFrontend frontend{{stillSize, stillSize}, 1};
    MapAdapter map{
        frontend, observer, fileSource, MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())};
    map.getStyle().loadJSON(sparseStyle(true, "terrain", false, /*hillshadeOnly=*/true));
    map.setCenterClampedToGround(true);
    map.jumpTo(CameraOptions().withCenter(flank).withZoom(arrivalZoom).withPitch(pitch).withBearing(0.0));
    const Still still = renderStillWithin(frontend, map);
    ASSERT_TRUE(still.finished) << "the still image waited on tiles the network can't load offline";
    ASSERT_FALSE(still.error);
    EXPECT_LT(backgroundShare(still.image), 0.5);
    const double share = mismatch(still.image, reference);
    EXPECT_LE(share, maxMismatch) << share * 100.0 << " % of the frame differs from the dense reference";
}

// Offline with no DEM tile at all: nothing to clamp to, so flat ground, as Task 35 has it.
TEST(TerrainSparseDEM, AnOfflineStillWithNoDEMIsFlat) {
    util::RunLoop loop;
    OfflineDEM database("test/fixtures/offline_database/terrain_sparse_still_none.db");
    OfflineNetwork offline;
    auto fileSource = std::make_shared<MainResourceLoader>(ResourceOptions().withCachePath(database.path),
                                                           ClientOptions());
    FrameObserver observer;
    HeadlessFrontend frontend{{stillSize, stillSize}, 1};
    MapAdapter map{
        frontend, observer, fileSource, MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())};
    map.getStyle().loadJSON(sparseStyle(true, "terrain", false, /*hillshadeOnly=*/true));
    map.setCenterClampedToGround(true);
    map.jumpTo(CameraOptions().withCenter(flank).withZoom(arrivalZoom).withPitch(pitch).withBearing(0.0));
    const Still still = renderStillWithin(frontend, map);
    ASSERT_TRUE(still.finished);
    ASSERT_FALSE(still.error);
    EXPECT_EQ(*map.getCameraOptions().centerAltitude, 0.0);
    EXPECT_GT(backgroundShare(still.image), 0.99);
}

TEST(TerrainSparseDEM, ZoomingInPastTheDataKeepsTheShapeOn204) {
    expectZoomInKeepsTheShape(Missing::NoContent);
}

TEST(TerrainSparseDEM, ZoomingInPastTheDataKeepsTheShapeOn404) {
    expectZoomInKeepsTheShape(Missing::NotFound);
}

TEST(TerrainSparseDEM, ZoomingWithinALevelPastTheDataKeepsTheShapeOn204) {
    expectZoomWithinALevelKeepsTheShape(Missing::NoContent);
}

TEST(TerrainSparseDEM, ZoomingWithinALevelPastTheDataKeepsTheShapeOn404) {
    expectZoomWithinALevelKeepsTheShape(Missing::NotFound);
}

TEST(TerrainSparseDEM, AColdJumpPastTheDataKeepsTheShapeOn204) {
    expectColdJumpKeepsTheShape(Missing::NoContent);
}

TEST(TerrainSparseDEM, AColdJumpPastTheDataKeepsTheShapeOn404) {
    expectColdJumpKeepsTheShape(Missing::NotFound);
}

// Two zooms past the data: at z15.5 the ideal tiles are z14 (overscaled), empty, and so are their
// z13 parents; the nearest data is z12. (maplibre-gl-js 6.16 draws this from a z5 tile on a 204.)
TEST(TerrainSparseDEM, ZoomingInTwoLevelsPastTheDataKeepsTheShapeOn204) {
    expectZoomInKeepsTheShape(Missing::NoContent, 15.5);
}

TEST(TerrainSparseDEM, AColdJumpTwoLevelsPastTheDataKeepsTheShapeOn204) {
    expectColdJumpKeepsTheShape(Missing::NoContent, 15.5);
}

// Where z13-14 tiles exist, they are drawn and queried, not their z12 parent: the bump only they
// hold is there, at z13.5 and at z14.5 (overzoomed from z14).
TEST(TerrainSparseDEM, TheSubBoxUsesItsOwnTiles) {
    for (const double zoom : {13.5, 14.5}) {
        SCOPED_TRACE("zoom " + std::to_string(zoom));
        SparseScene scene;
        scene.jumpTo(scene.camera(bump, zoom, 0.0));
        const auto meters = scene.elevation(bump);
        ASSERT_TRUE(meters);
        EXPECT_NEAR(*meters, coneMeters(bump) + bumpMeters, 1.0);
    }
}

// A tile known to be empty is asked for once a session: not again when the view comes back to it,
// nor after the tile cache was dropped. A tile the map sees for the first time is asked for once.
TEST(TerrainSparseDEM, AnEmptyTileIsRequestedOnce) {
    SparseScene scene;
    scene.jumpTo(scene.camera(flank, arrivalZoom, pitch));
    ASSERT_GT(scene.requestsAbove(planetMaxZoom), 0);
    // Out to z11 and back, then out again with the tile cache dropped (as on a memory warning) and back.
    scene.jumpTo(scene.camera(flank, 11.0, pitch));
    scene.jumpTo(scene.camera(flank, arrivalZoom, pitch));
    scene.jumpTo(scene.camera(flank, 11.0, pitch));
    scene.frontend.getRenderer()->reduceMemoryUse();
    scene.jumpTo(scene.camera(flank, arrivalZoom, pitch));
    for (const auto& [id, n] : scene.networkRequests) {
        if (id.z > planetMaxZoom) {
            EXPECT_EQ(n, 1) << "z" << int(id.z) << "/" << id.x << "/" << id.y << " was requested " << n << " times";
        }
    }
    expectTheCone(scene, "back at z13.5");
}

// How an empty answer's Cache-Control is honoured when the view comes back to its tiles, with the
// same tile objects (from the tile cache) and with new ones (the tile cache dropped): an answer
// that may not be reused is asked for again from the network each time, one that may is not.
// Out to here and back. The tile cache keeps no tile without data (TileCache::add), so the view's
// empty z13 tiles are made anew on the way back.
constexpr double outZoom = 12.0;

void expectEmptyAnswersReused(const std::string& cacheControl, bool reused) {
    SCOPED_TRACE("Cache-Control: " + cacheControl);
    SceneOptions options;
    options.emptyCacheControl = cacheControl;
    SparseScene scene(options);
    scene.jumpTo(scene.camera(flank, arrivalZoom, pitch));
    const auto [x, y] = tileCoordinates(flank);
    const CanonicalTileID under(planetMaxZoom + 1, static_cast<uint32_t>(x * 2), static_cast<uint32_t>(y * 2));
    ASSERT_EQ(scene.networkRequests[under], 1) << "the view does not ask for the tile under its centre";
    // Out to z12 and back: the empty tiles are made anew (the tile cache keeps none).
    scene.jumpTo(scene.camera(flank, outZoom, pitch));
    scene.jumpTo(scene.camera(flank, arrivalZoom, pitch));
    EXPECT_EQ(scene.networkRequests[under], reused ? 1 : 2) << "back";
    scene.jumpTo(scene.camera(flank, 11.0, pitch));
    scene.frontend.getRenderer()->reduceMemoryUse();
    scene.jumpTo(scene.camera(flank, arrivalZoom, pitch));
    EXPECT_EQ(scene.networkRequests[under], reused ? 1 : 3) << "back after the tile cache was dropped";
    expectTheCone(scene, "back at z13.5");
}

TEST(TerrainSparseDEM, AnEmptyAnswerThatMayNotBeReusedIsAskedForAgain) {
    expectEmptyAnswersReused("max-age=0", false);
    expectEmptyAnswersReused("no-cache", false);
    expectEmptyAnswersReused("public, no-store", false);
}

TEST(TerrainSparseDEM, AnEmptyAnswerThatMayBeReusedIsNotAskedForAgain) {
    expectEmptyAnswersReused("max-age=3600", true);
}

// A tile known to be empty is still asked for from the cache when the view comes back to it, as
// before: never from the network while the record holds, but data an offline pack brought since
// (and the server has too) is drawn. The tile cache keeps no tile without data (TileCache::add),
// so the tile is made anew on the way back, and a new tile asks the cache first; a tile required
// again as the same object (held as an optional parent or child meanwhile) does too, see
// RasterDEMTile.AKnownEmptyTileRequiredAgainAsksOnlyTheCache.
TEST(TerrainSparseDEM, ARevisitAsksTheCacheAgain) {
    SparseScene scene;
    scene.jumpTo(scene.camera(flank, arrivalZoom, 0.0));
    const auto [x, y] = tileCoordinates(flank);
    const CanonicalTileID under(planetMaxZoom + 1, static_cast<uint32_t>(x * 2), static_cast<uint32_t>(y * 2));
    ASSERT_EQ(scene.networkRequests[under], 1);
    const auto outAndBack = [&] {
        scene.jumpTo(scene.camera(flank, outZoom, 0.0));
        scene.jumpTo(scene.camera(flank, arrivalZoom, 0.0));
    };
    const int cacheAsked = scene.cacheRequests[under];
    outAndBack();
    EXPECT_GT(scene.cacheRequests[under], cacheAsked) << "the revisit did not ask the cache";
    EXPECT_LE(scene.cacheRequests[under], cacheAsked + 2) << "the cache is asked on every frame";
    EXPECT_EQ(scene.networkRequests[under], 1) << "the revisit asked the network while the record holds";

    scene.cached[under] = Ground::RaisedCone;
    scene.gained[under] = Ground::RaisedCone;
    outAndBack();
    const auto meters = scene.elevation(flank);
    ASSERT_TRUE(meters);
    EXPECT_NEAR(*meters, coneMeters(flank) + raisedBy, 0.25) << "the offline pack's data is not drawn";
}

// What the map learnt from empty answers lapses when the cache or offline database changes as a
// whole (invalidated, cleared, a region deleted...): the tile is asked for from the network again.
TEST(TerrainSparseDEM, AChangedCacheForgetsTheEmptyTiles) {
    SparseScene scene;
    scene.jumpTo(scene.camera(flank, arrivalZoom, 0.0));
    const auto [x, y] = tileCoordinates(flank);
    const CanonicalTileID under(planetMaxZoom + 1, static_cast<uint32_t>(x * 2), static_cast<uint32_t>(y * 2));
    ASSERT_EQ(scene.networkRequests[under], 1);
    scene.jumpTo(scene.camera(flank, outZoom, 0.0));
    bumpAmbientCacheGeneration();
    scene.jumpTo(scene.camera(flank, arrivalZoom, 0.0));
    EXPECT_EQ(scene.networkRequests[under], 2);
}

// Map::pixelForLatLng, but for a point `meters` above sea level instead of on it.
ScreenCoordinate project(const TransformState& state, const LatLng& latLng, double meters) {
    const Point<double> pt = Projection::project(latLng, state.getScale());
    const vec4 c = {{pt.x, pt.y, meters, 1}};
    vec4 p;
    matrix::transformMat4(p, c, state.getProjectionMatrix());
    const Size size = state.getSize();
    return {(p[0] / p[3] + 1) * size.width / 2, size.height - (p[1] / p[3] + 1) * size.height / 2};
}

// The other consumers of the terrain's height, outside the sub-box, once the ancestor has loaded:
// the camera clamp lifts the centre onto the cone, and a marker is drawn, and found, at the cone's
// height. (On a cold jump the clamp reads 0 m until the ancestor chain has loaded, one load per
// level: the scene waits for that.)
void expectTheAncestorsHeightUsedByMarkersAndTheClamp(double zoom, bool zoomIn) {
    SCOPED_TRACE(std::string(zoomIn ? "zoomed in to z" : "jumped to z") + std::to_string(zoom));
    SceneOptions options;
    options.marker = true;
    SparseScene scene(options);
    scene.map.setCenterClampedToGround(true);
    const auto camera = [&](double z) {
        return CameraOptions().withCenter(flank).withZoom(z).withPitch(pitch).withBearing(0.0);
    };
    if (zoomIn) {
        scene.jumpTo(camera(planetMaxZoom));
        scene.easeTo(camera(zoom));
    } else {
        scene.jumpTo(camera(zoom));
    }
    // The clamp follows the ground over a few frames.
    for (int i = 0; i < 8; ++i) {
        scene.map.triggerRepaint();
        scene.settle();
    }
    const auto centre = scene.map.getCameraOptions({}).centerAltitude;
    ASSERT_TRUE(centre);
    EXPECT_NEAR(*centre, coneMeters(flank), 5.0) << "the clamp does not hold the centre on the cone";

    const auto state = scene.frontend.getTransformState();
    ASSERT_TRUE(state);
    const ScreenCoordinate drawn = project(*state, marker, coneMeters(marker));
    const ScreenCoordinate seaLevel = project(*state, marker, 0.0);
    ASSERT_GT(drawn.x, 10);
    ASSERT_GT(drawn.y, 10);
    ASSERT_LT(drawn.x, viewWidth - 10);
    ASSERT_LT(drawn.y, viewHeight - 10);
    ASSERT_GT(std::hypot(drawn.x - seaLevel.x, drawn.y - seaLevel.y), 40.0) << "the test proves nothing";
    const PremultipliedImage image = scene.image();
    int red = 0;
    for (int dy = -3; dy <= 3; ++dy) {
        for (int dx = -3; dx <= 3; ++dx) {
            const uint8_t* px = image.data.get() + (static_cast<size_t>(drawn.y + dy) * image.size.width +
                                                    static_cast<size_t>(drawn.x + dx)) *
                                                       4;
            red += px[0] > 150 && px[1] < 100 && px[2] < 100;
        }
    }
    EXPECT_EQ(red, 49) << "the marker is not drawn at the cone's height";
    RenderedQueryOptions query;
    query.layerIDs = std::vector<std::string>{"marker"};
    EXPECT_EQ(scene.frontend.getRenderer()->queryRenderedFeatures(drawn, query).size(), 1u)
        << "the marker is not found where it is drawn";
}

TEST(TerrainSparseDEM, MarkersAndTheCameraClampUseTheAncestorsHeight) {
    expectTheAncestorsHeightUsedByMarkersAndTheClamp(arrivalZoom, false);
    expectTheAncestorsHeightUsedByMarkersAndTheClamp(15.5, false);
    expectTheAncestorsHeightUsedByMarkersAndTheClamp(15.5, true);
}

// The cut-out: a z11 tile that straddles the coast holds 0 m pixels on the cut's side, and every
// tile wholly inside the cut answers empty, z12 included. At every zoom the cut is drawn and
// queried at 0 m from the nearest real ancestor, and the real ground beside it keeps its heights,
// with no step but the coast's.
TEST(TerrainSparseDEM, ACutOutRegionStaysAtSeaLevelAtEveryZoom) {
    const double kmPerLng = kmPerDegree * std::cos(util::deg2rad(apex.latitude()));
    // Half way down the cut's west coast, a z12 tile from its north and south coasts: the only
    // coast in view at every zoom below.
    const LatLng coast = tilePoint(coastX, apexY + 2.0);
    for (const double zoom : {11.5, 12.5, 13.5, 14.5}) {
        SCOPED_TRACE("zoom " + std::to_string(zoom));
        SceneOptions options;
        options.ground = Ground::ConeWithCut;
        SparseScene scene(options);
        scene.jumpTo(scene.camera(coast, zoom, 0.0));
        // Across the coast, from 1 km west (real ground) to 1 km east (the cut), but not within a z11
        // DEM pixel (26 m) of it, where the z11 tile's bilinear surface runs down the cliff.
        for (const double eastKm : {-1.0, -0.5, -0.2, -0.05, 0.05, 0.2, 0.5, 1.0}) {
            const LatLng point(coast.latitude(), coast.longitude() + eastKm / kmPerLng);
            const auto meters = scene.elevation(point);
            ASSERT_TRUE(meters) << "no elevation " << eastKm << " km east of the coast";
            EXPECT_NEAR(*meters, eastKm < 0 ? coneMeters(point) : 0.0, 0.25) << eastKm << " km east of the coast";
        }
        // Drawn as a tileset with the cut's tiles at 0 m would draw it.
        SceneOptions denseOptions = options;
        denseOptions.tileset = TilesetKind::Dense;
        SparseScene dense(denseOptions);
        dense.jumpTo(dense.camera(coast, zoom, 0.0));
        const double share = mismatch(scene.image(), dense.image(), coastBand(zoom));
        EXPECT_LE(share, maxCoastMismatch) << share * 100.0 << " % of the frame away from the coast differs";
    }
}

// A tile known to be empty that gains data - from the cache (an offline pack downloaded since, say)
// and from the server, which now has it - draws it, and is no longer known to be empty: created
// anew, with the cache cleared, it is asked for again and its data drawn.
TEST(TerrainSparseDEM, ATileThatGainsDataIsNoLongerKnownEmpty) {
    SparseScene scene;
    scene.jumpTo(scene.camera(flank, arrivalZoom, 0.0));
    const auto [x, y] = tileCoordinates(flank);
    const CanonicalTileID under(planetMaxZoom + 1, static_cast<uint32_t>(x * 2), static_cast<uint32_t>(y * 2));
    ASSERT_EQ(scene.networkRequests[under], 1) << "the view does not ask for the tile under its centre";
    const auto leaveAndComeBack = [&] {
        scene.jumpTo(scene.camera(flank, 11.0, 0.0));
        scene.frontend.getRenderer()->reduceMemoryUse();
        scene.jumpTo(scene.camera(flank, arrivalZoom, 0.0));
    };
    const auto expectRaised = [&](const std::string& when) {
        const auto meters = scene.elevation(flank);
        ASSERT_TRUE(meters) << when;
        EXPECT_NEAR(*meters, coneMeters(flank) + raisedBy, 0.25) << when << ": the tile's data is not drawn";
    };

    // The cache answers first, then the network revalidates what it answered.
    scene.cached[under] = Ground::RaisedCone;
    scene.gained[under] = Ground::RaisedCone;
    leaveAndComeBack();
    EXPECT_EQ(scene.networkRequests[under], 2);
    expectRaised("with data in the cache");

    scene.cached.clear();
    leaveAndComeBack();
    EXPECT_EQ(scene.networkRequests[under], 3) << "a tile that has data is still taken for empty";
    expectRaised("with the cache cleared");
}

// What the tiles of one tileset answered says nothing of another's: a style that moves the source
// to a new tileset asks for every tile of it.
TEST(TerrainSparseDEM, ANewTilesetForgetsTheEmptyTiles) {
    SparseScene scene;
    scene.jumpTo(scene.camera(flank, arrivalZoom, 0.0));
    const int before = scene.requestsAbove(planetMaxZoom);
    ASSERT_GT(before, 0);
    scene.map.getStyle().loadJSON(sparseStyle(true, "terrain-v2"));
    scene.jumpTo(scene.camera(flank, arrivalZoom, 0.0));
    EXPECT_GT(scene.requestsAbove(planetMaxZoom), before) << "the new tileset's tiles were not asked for";
    const auto meters = scene.elevation(flank);
    ASSERT_TRUE(meters);
    EXPECT_NEAR(*meters, coneMeters(flank) + raisedBy, 0.25) << "the new tileset's tile is not drawn";
}

// The record of empty tiles: an answer with no expiry holds for the session, one with an expiry
// until then; data drops it; and it stays bounded.
TEST(TerrainSparseDEM, TheKnownEmptyRecord) {
    KnownEmptyDEMTiles record;
    const CanonicalTileID a(13, 4328, 2895);
    const CanonicalTileID b(13, 4328, 2896);
    record.add(a, std::nullopt);
    record.add(b, std::chrono::time_point_cast<Seconds>(util::now() + std::chrono::hours(1)));
    EXPECT_TRUE(record.contains(a));
    EXPECT_TRUE(record.contains(b));
    record.add(b, std::chrono::time_point_cast<Seconds>(util::now() - std::chrono::seconds(1)));
    EXPECT_FALSE(record.contains(b)) << "an expired answer still holds";
    EXPECT_EQ(record.size(), 1u) << "an answer that may not be reused is recorded";
    record.remove(a);
    EXPECT_FALSE(record.contains(a));
    for (uint32_t i = 0; i < 3 * KnownEmptyDEMTiles::maxEntries; ++i) {
        record.add(CanonicalTileID(14, i, 0), std::nullopt);
        ASSERT_LE(record.size(), KnownEmptyDEMTiles::maxEntries);
    }
    // A change of the cache as a whole drops everything.
    record.add(a, std::nullopt);
    ASSERT_TRUE(record.contains(a));
    bumpAmbientCacheGeneration();
    EXPECT_FALSE(record.contains(a));
    EXPECT_EQ(record.size(), 0u);
}

// At the cap, the expired entries go first: the ones that still hold are kept.
TEST(TerrainSparseDEM, TheKnownEmptyRecordDropsExpiredEntriesAtItsCap) {
    KnownEmptyDEMTiles record;
    const CanonicalTileID kept(13, 1, 1);
    record.add(kept, std::nullopt);
    const auto soon = std::chrono::time_point_cast<Seconds>(util::now() + std::chrono::seconds(1));
    for (uint32_t i = 0; record.size() < KnownEmptyDEMTiles::maxEntries; ++i) {
        record.add(CanonicalTileID(14, i, 0), soon);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    record.add(CanonicalTileID(13, 2, 2), std::nullopt);
    EXPECT_TRUE(record.contains(kept)) << "the record started over though expired entries made room";
    EXPECT_EQ(record.size(), 2u);
}
