#include <mln/test/util.hpp>
#include <mln/test/fixture_log_observer.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/storage/main_resource_loader.hpp>
#include <mln/storage/network_status.hpp>
#include <mln/storage/offline_database.hpp>
#include <mln/storage/resource.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/storage/response.hpp>
#include <mln/style/style.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/util/geo.hpp>
#include <mln/util/image.hpp>
#include <mln/util/io.hpp>
#include <mln/util/async_request.hpp>
#include <mln/util/async_task.hpp>
#include <mln/util/chrono.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/tile_server_options.hpp>
#include <mln/util/timer.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace mln;

// A still image render offline (backlog B9). A still image waits for every resource it asks for.
// While the network status is Offline, the online file source holds a request that the cache
// can't answer until the network is back, so a still that asked for one never finished: the
// snapshot of a view at the edge of a downloaded region froze the app.
//
// A tile the cache doesn't have is drawn as a tile with no data while the network is offline, so
// the still finishes with what the cache has. A resource the still can't be drawn without (the
// style, a glyph range, the sprite, a TileJSON, an image or GeoJSON source) fails the still, as
// such a resource does online when it can't be loaded.
//
// Every still is waited for at most stillLimit: one that never finishes fails the test instead of
// stalling the run. These stills take milliseconds.

namespace {

constexpr auto stillLimit = std::chrono::seconds(20);
constexpr uint32_t viewSize = 256;

const std::string vectorURL = "http://tiles.invalid/vector/{z}/{x}/{y}.pbf";
const std::string styleURL = "http://tiles.invalid/still-style.json";
// A z13 tile; the camera looks at its middle from zoom 14, so the four z14 tiles in view are its
// children.
const CanonicalTileID parentTile{13, 4340, 2915};
const LatLng parentMiddle = LatLngBounds(parentTile).center();

struct Still {
    bool finished = false;
    std::exception_ptr error;
    PremultipliedImage image;
};

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

std::string message(const std::exception_ptr& error) {
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "unknown";
    }
}

// --- A minimal Mapbox Vector Tile: one layer, "area", with one polygon over the whole tile. ---
void varint(std::string& out, uint64_t value) {
    while (value >= 0x80) {
        out += static_cast<char>((value & 0x7F) | 0x80);
        value >>= 7;
    }
    out += static_cast<char>(value);
}
void fieldKey(std::string& out, uint32_t field, uint32_t wireType) {
    varint(out, (field << 3) | wireType);
}
void bytesField(std::string& out, uint32_t field, const std::string& data) {
    fieldKey(out, field, 2);
    varint(out, data.size());
    out += data;
}
uint32_t zigzag(int32_t value) {
    return (static_cast<uint32_t>(value) << 1) ^ static_cast<uint32_t>(value >> 31);
}

std::string areaTile() {
    constexpr int extent = 4096;
    constexpr int lo = -64;
    constexpr int hi = extent + 64;
    std::string geometry;
    varint(geometry, 1 | (1 << 3)); // MoveTo, 1
    varint(geometry, zigzag(lo));
    varint(geometry, zigzag(lo));
    varint(geometry, 2 | (3 << 3)); // LineTo, 3: clockwise with y down, an exterior ring
    varint(geometry, zigzag(hi - lo));
    varint(geometry, zigzag(0));
    varint(geometry, zigzag(0));
    varint(geometry, zigzag(hi - lo));
    varint(geometry, zigzag(lo - hi));
    varint(geometry, zigzag(0));
    varint(geometry, 7 | (1 << 3)); // ClosePath
    std::string feature;
    fieldKey(feature, 3, 0);
    varint(feature, 3); // POLYGON
    bytesField(feature, 4, geometry);
    std::string layer;
    fieldKey(layer, 15, 0);
    varint(layer, 2);
    bytesField(layer, 1, "area");
    bytesField(layer, 2, feature);
    fieldKey(layer, 5, 0);
    varint(layer, extent);
    std::string tile;
    bytesField(tile, 3, layer);
    return tile;
}

const std::string vectorStyle = R"STYLE({
  "version": 8,
  "sources": {
    "vector": {"type": "vector", "tiles": [")STYLE" +
                                vectorURL + R"STYLE("], "maxzoom": 14}
  },
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "#f4f1ea"}},
    {"id": "area", "type": "fill", "source": "vector", "source-layer": "area",
     "paint": {"fill-color": "#2a7ab0", "fill-antialias": false}}
  ]
})STYLE";

Response dataResponse(const std::string& data) {
    Response res;
    res.data = std::make_shared<std::string>(data);
    return res;
}

class OfflineFile {
public:
    explicit OfflineFile(std::string path_)
        : path(std::move(path_)) {
        remove();
    }
    ~OfflineFile() { remove(); }
    OfflineFile(const OfflineFile&) = delete;
    OfflineFile& operator=(const OfflineFile&) = delete;

    void put(const Resource& resource, const Response& response) {
        OfflineDatabase db(path, TileServerOptions::DefaultConfiguration());
        db.put(resource, response);
    }

    const std::string path;

private:
    void remove() {
        util::deleteFile(path);
        util::deleteFile(path + "-wal");
        util::deleteFile(path + "-journal");
    }
};

struct OfflineNetwork {
    OfflineNetwork() { NetworkStatus::Set(NetworkStatus::Status::Offline); }
    ~OfflineNetwork() { NetworkStatus::Set(NetworkStatus::Status::Online); }
    OfflineNetwork(const OfflineNetwork&) = delete;
    OfflineNetwork& operator=(const OfflineNetwork&) = delete;
};

// A static map over the engine's own loader on `database`, with the network offline.
struct OfflineStill {
    OfflineNetwork offline;
    std::shared_ptr<FileSource> fileSource;
    HeadlessFrontend frontend{{viewSize, viewSize}, 1};
    MapAdapter map;

    explicit OfflineStill(const OfflineFile& database)
        : fileSource(
              std::make_shared<MainResourceLoader>(ResourceOptions().withCachePath(database.path), ClientOptions())),
          map(frontend,
              MapObserver::nullObserver(),
              fileSource,
              MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())) {}

    Still render() { return renderStillWithin(frontend, map, stillLimit); }
};

int countColour(const PremultipliedImage& image, uint8_t r, uint8_t g, uint8_t b) {
    int count = 0;
    for (size_t i = 0; i < image.bytes(); i += 4) {
        const uint8_t* px = image.data.get() + i;
        count += std::abs(px[0] - r) <= 4 && std::abs(px[1] - g) <= 4 && std::abs(px[2] - b) <= 4 ? 1 : 0;
    }
    return count;
}

// Expects a still that the offline network status can't hold: it finishes, and fails, as the
// resource it can't be drawn without is missing.
void expectTheStillFails(OfflineStill& scene) {
    const Still still = scene.render();
    ASSERT_TRUE(still.finished) << "the still image waited for more than " << stillLimit.count()
                                << " s on a resource the network can't load offline";
    ASSERT_TRUE(still.error) << "the still image finished without a resource it needs";
    EXPECT_NE(std::string::npos, message(still.error).find("Online connectivity is disabled")) << message(still.error);
}

} // namespace

// The z14 tiles in view are missing; their z13 parent is in the database, so the still draws it.
TEST(StillImageOffline, AMissingVectorTileDrawsItsAncestor) {
    util::RunLoop loop;
    OfflineFile database("test/fixtures/offline_database/still_image_offline_vector.db");
    database.put(Resource::style(styleURL), dataResponse(vectorStyle));
    database.put(Resource::tile(vectorURL, 1, parentTile.x, parentTile.y, parentTile.z, Tileset::Scheme::XYZ),
                 dataResponse(areaTile()));

    OfflineStill scene(database);
    scene.map.getStyle().loadURL(styleURL);
    scene.map.jumpTo(CameraOptions().withCenter(parentMiddle).withZoom(14));
    const Still still = scene.render();
    ASSERT_TRUE(still.finished) << "the still image waited for more than " << stillLimit.count()
                                << " s on vector tiles the network can't load offline";
    ASSERT_FALSE(still.error) << message(still.error);
    EXPECT_EQ(countColour(still.image, 0x2a, 0x7a, 0xb0), static_cast<int>(viewSize * viewSize))
        << "the z13 parent is not drawn in place of its missing children";
}

// Nothing of the vector source is in the database: the still finishes without it.
TEST(StillImageOffline, AMissingVectorTileWithNoAncestorFinishesWithoutIt) {
    util::RunLoop loop;
    OfflineFile database("test/fixtures/offline_database/still_image_offline_vector_none.db");
    database.put(Resource::style(styleURL), dataResponse(vectorStyle));

    OfflineStill scene(database);
    scene.map.getStyle().loadURL(styleURL);
    scene.map.jumpTo(CameraOptions().withCenter(parentMiddle).withZoom(14));
    const Still still = scene.render();
    ASSERT_TRUE(still.finished) << "the still image waited for more than " << stillLimit.count()
                                << " s on vector tiles the network can't load offline";
    ASSERT_FALSE(still.error) << message(still.error);
    EXPECT_EQ(countColour(still.image, 0xf4, 0xf1, 0xea), static_cast<int>(viewSize * viewSize));
}

TEST(StillImageOffline, AMissingStyleFailsTheStill) {
    util::RunLoop loop;
    OfflineFile database("test/fixtures/offline_database/still_image_offline_style.db");
    OfflineStill scene(database);
    scene.map.getStyle().loadURL(styleURL);
    expectTheStillFails(scene);
}

TEST(StillImageOffline, AMissingGlyphRangeFailsTheStill) {
    util::RunLoop loop;
    OfflineFile database("test/fixtures/offline_database/still_image_offline_glyphs.db");
    OfflineStill scene(database);
    scene.map.getStyle().loadJSON(R"({
      "version": 8,
      "glyphs": "http://tiles.invalid/fonts/{fontstack}/{range}.pbf",
      "sources": {"point": {"type": "geojson", "data": {"type": "Point", "coordinates": [0, 0]}}},
      "layers": [{"id": "label", "type": "symbol", "source": "point", "layout": {"text-field": "A"}}]
    })");
    expectTheStillFails(scene);
}

TEST(StillImageOffline, AMissingSpriteFailsTheStill) {
    util::RunLoop loop;
    OfflineFile database("test/fixtures/offline_database/still_image_offline_sprite.db");
    OfflineStill scene(database);
    scene.map.getStyle().loadJSON(R"({
      "version": 8,
      "sprite": "http://tiles.invalid/sprites/sprite",
      "sources": {},
      "layers": [{"id": "background", "type": "background", "paint": {"background-color": "#f4f1ea"}}]
    })");
    expectTheStillFails(scene);
}

TEST(StillImageOffline, AMissingTileJSONFailsTheStill) {
    util::RunLoop loop;
    OfflineFile database("test/fixtures/offline_database/still_image_offline_tilejson.db");
    OfflineStill scene(database);
    scene.map.getStyle().loadJSON(R"({
      "version": 8,
      "sources": {"vector": {"type": "vector", "url": "http://tiles.invalid/tiles.json"}},
      "layers": [{"id": "area", "type": "fill", "source": "vector", "source-layer": "area"}]
    })");
    expectTheStillFails(scene);
}

TEST(StillImageOffline, AMissingImageSourceFailsTheStill) {
    util::RunLoop loop;
    OfflineFile database("test/fixtures/offline_database/still_image_offline_image.db");
    OfflineStill scene(database);
    scene.map.getStyle().loadJSON(R"({
      "version": 8,
      "sources": {"image": {"type": "image", "url": "http://tiles.invalid/image.png",
                  "coordinates": [[-1, 1], [1, 1], [1, -1], [-1, -1]]}},
      "layers": [{"id": "image", "type": "raster", "source": "image"}]
    })");
    expectTheStillFails(scene);
}

TEST(StillImageOffline, AMissingGeoJSONFailsTheStill) {
    util::RunLoop loop;
    OfflineFile database("test/fixtures/offline_database/still_image_offline_geojson.db");
    OfflineStill scene(database);
    scene.map.getStyle().loadJSON(R"({
      "version": 8,
      "sources": {"data": {"type": "geojson", "data": "http://tiles.invalid/data.geojson"}},
      "layers": [{"id": "line", "type": "line", "source": "data"}]
    })");
    expectTheStillFails(scene);
}

// --- What the fix must not change, against a cache and a network that follow the online file
// source's offline contract (OnlineFileSource.Offline* tests that contract itself). ---

namespace {

// A cache and a network, as MainResourceLoader over OnlineFileSource: a cache-only request is
// answered from `cache`, and a network request from `server` after `latency`. While NetworkStatus
// is Offline a network request is held until the network is back, and answered then. A request
// whose resource asks to hear that the network is offline (Resource::reportOffline) gets a
// Connection error first, as does every held one with `failHeldRequests` (as a request that was in
// flight when the network went away).
class CacheAndNetwork final : public FileSource {
public:
    using Answer = std::function<std::optional<Response>(const Resource&)>;

    CacheAndNetwork() {
        NetworkStatus::Subscribe(&reachable);
        timer.start(Milliseconds(1), Milliseconds(1), [this] { answerDue(); });
    }
    ~CacheAndNetwork() override { NetworkStatus::Unsubscribe(&reachable); }

    Answer cache;
    Answer server;
    Duration latency = Duration::zero();
    bool failHeldRequests = false;

    // Every network request, in order.
    std::vector<Resource> networkRequests;
    int offlineAnswers = 0;
    size_t heldRequests() const { return held.size(); }

    std::unique_ptr<AsyncRequest> request(const Resource& resource, Callback callback) override {
        auto req = std::make_unique<Request>(*this, resource, std::move(callback));
        if (resource.loadingMethod == Resource::LoadingMethod::CacheOnly) {
            const auto cached = cache ? cache(resource) : std::nullopt;
            answer(*req, cached ? *cached : notFound(), Duration::zero());
            return req;
        }
        networkRequests.push_back(resource);
        if (NetworkStatus::Get() == NetworkStatus::Status::Offline) {
            held.push_back(req.get());
            if (resource.reportOffline || failHeldRequests) {
                ++offlineAnswers;
                Response offline;
                offline.error = std::make_unique<Response::Error>(Response::Error::Reason::Connection,
                                                                  "Online connectivity is disabled.");
                answer(*req, offline, Duration::zero());
            }
        } else {
            answer(*req, fromServer(resource), latency);
        }
        return req;
    }

    bool supportsCacheOnlyRequests() const override { return true; }
    bool canRequest(const Resource&) const override { return true; }
    void setResourceOptions(ResourceOptions options) override { resourceOptions = std::move(options); }
    ResourceOptions getResourceOptions() override { return resourceOptions.clone(); }
    void setClientOptions(ClientOptions options) override { clientOptions = std::move(options); }
    ClientOptions getClientOptions() override { return clientOptions.clone(); }

private:
    class Request final : public AsyncRequest {
    public:
        Request(CacheAndNetwork& network_, Resource resource_, Callback callback_)
            : network(network_),
              resource(std::move(resource_)),
              callback(std::move(callback_)) {}
        ~Request() override { network.forget(this); }
        CacheAndNetwork& network;
        const Resource resource;
        const Callback callback;
    };

    struct Due {
        Request* request;
        Response response;
        TimePoint at;
    };

    static Response notFound() {
        Response res;
        res.noContent = true;
        res.error = std::make_unique<Response::Error>(Response::Error::Reason::NotFound, "not cached");
        return res;
    }

    Response fromServer(const Resource& resource) const {
        if (auto res = server ? server(resource) : std::nullopt) return *res;
        Response empty;
        empty.noContent = true;
        return empty;
    }

    void answer(Request& req, Response res, Duration after) {
        due.push_back(Due{.request = &req, .response = std::move(res), .at = Clock::now() + after});
    }

    void forget(Request* req) {
        std::erase_if(due, [&](const Due& d) { return d.request == req; });
        std::erase(held, req);
    }

    void answerDue() {
        // An answer can drop its request, or any other: look again after each.
        for (bool answered = true; answered;) {
            answered = false;
            for (auto it = due.begin(); it != due.end(); ++it) {
                if (it->at <= Clock::now()) {
                    Request* req = it->request;
                    Response res = std::move(it->response);
                    due.erase(it);
                    const Callback callback = req->callback;
                    callback(res);
                    answered = true;
                    break;
                }
            }
        }
    }

    void onReachable() {
        if (NetworkStatus::Get() != NetworkStatus::Status::Online) return;
        for (Request* req : held) {
            answer(*req, fromServer(req->resource), latency);
        }
        held.clear();
    }

    ResourceOptions resourceOptions;
    ClientOptions clientOptions;
    std::vector<Request*> held;
    std::list<Due> due;
    util::Timer timer;
    util::AsyncTask reachable{[this] {
        onReachable();
    }};
};

// Terrain over a DEM that the cache has to z12 (300 m) and the network at z13 (600 m), so the
// height under the centre says which one is drawn.
const std::string demURL = "http://tiles.invalid/dem/{z}/{x}/{y}.png";
constexpr double ancestorMeters = 300.0;
constexpr double tileMeters = 600.0;
const LatLng demCentre = LatLngBounds(CanonicalTileID(13, 4340, 2915)).center();

std::string demTile(double meters) {
    // Terrain-RGB: -10000 + (R * 65536 + G * 256 + B) * 0.1.
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

const std::string demStyle = R"STYLE({
  "version": 8,
  "sources": {
    "dem": {"type": "raster-dem", "tiles": [")STYLE" +
                             demURL + R"STYLE("], "maxzoom": 13, "tileSize": 256}
  },
  "terrain": {"source": "dem", "exaggeration": 1},
  "layers": [{"id": "background", "type": "background", "paint": {"background-color": "#f4f1ea"}}]
})STYLE";

bool isDEMTile(const Resource& resource, int z) {
    return resource.tileData && resource.tileData->urlTemplate == demURL && resource.tileData->z == z;
}

std::shared_ptr<CacheAndNetwork> demCacheAndNetwork() {
    auto network = std::make_shared<CacheAndNetwork>();
    network->cache = [ancestor = demTile(ancestorMeters)](const Resource& resource) -> std::optional<Response> {
        if (resource.tileData && resource.tileData->z <= 12) return dataResponse(ancestor);
        return std::nullopt;
    };
    network->server = [tile = demTile(tileMeters)](const Resource& resource) -> std::optional<Response> {
        if (isDEMTile(resource, 13)) return dataResponse(tile);
        return std::nullopt;
    };
    return network;
}

struct DEMScene {
    std::shared_ptr<CacheAndNetwork> network;
    HeadlessFrontend frontend{{viewSize, viewSize}, 1};
    MapAdapter map;

    DEMScene(std::shared_ptr<CacheAndNetwork> network_, MapMode mode)
        : network(std::move(network_)),
          map(frontend,
              MapObserver::nullObserver(),
              network,
              MapOptions().withMapMode(mode).withSize(frontend.getSize())) {
        map.getStyle().loadJSON(demStyle);
        at(13);
    }

    void at(double zoom) { map.jumpTo(CameraOptions().withCenter(demCentre).withZoom(zoom).withCenterAltitude(0)); }

    Still render() { return renderStillWithin(frontend, map, stillLimit); }

    std::optional<double> elevation() { return frontend.getRenderer()->queryTerrainElevation(demCentre); }

    int z13Requests() const {
        return static_cast<int>(
            std::ranges::count_if(network->networkRequests, [](const Resource& r) { return isDEMTile(r, 13); }));
    }

    // Runs a continuous map until `done`, for at most `limit`.
    bool runUntil(const std::function<bool()>& done, std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            util::RunLoop::Get()->runOnce();
        }
        return done();
    }
};

// The tile load errors logged since the last call.
std::string offlineTileErrors(const FixtureLog& log) {
    std::string errors;
    for (const auto& m : log.unchecked()) {
        if (m.severity == EventSeverity::Error && m.msg.find("Failed to load tile") != std::string::npos) {
            errors += m.msg + "\n";
        }
    }
    return errors;
}

} // namespace

// Offline, the still draws the z12 ancestor. Once the network is back, the next still asks for the
// z13 tile and waits for it, though the network takes a while: it draws the z13 tile.
TEST(StillImageOffline, TheTileArrivesWhenTheNetworkIsBack) {
    util::RunLoop loop;
    auto network = demCacheAndNetwork();
    network->latency = Milliseconds(300);
    DEMScene scene(network, MapMode::Static);
    {
        OfflineNetwork offline;
        const Still still = scene.render();
        ASSERT_TRUE(still.finished);
        ASSERT_FALSE(still.error) << message(still.error);
        ASSERT_TRUE(scene.elevation());
        EXPECT_NEAR(*scene.elevation(), ancestorMeters, 1.0);
        EXPECT_GT(scene.z13Requests(), 0);
        EXPECT_GT(network->heldRequests(), 0u) << "the z13 request is not held for the network's return";
    }
    const Still still = scene.render();
    ASSERT_TRUE(still.finished);
    ASSERT_FALSE(still.error) << message(still.error);
    ASSERT_TRUE(scene.elevation());
    EXPECT_NEAR(*scene.elevation(), tileMeters, 1.0) << "the still finished before the z13 tile arrived";
    EXPECT_EQ(network->heldRequests(), 0u);
}

// Offline is not "no data": a tile missing offline is asked for again each time it is needed, where
// a tile that answered empty is asked for once (TerrainSparseDEM.AnEmptyTileIsRequestedOnce).
TEST(StillImageOffline, AMissingTileIsAskedForAgainWhenItIsNeededAgain) {
    util::RunLoop loop;
    auto network = demCacheAndNetwork();
    DEMScene scene(network, MapMode::Static);
    OfflineNetwork offline;
    ASSERT_TRUE(scene.render().finished);
    const int first = scene.z13Requests();
    ASSERT_GT(first, 0);
    scene.at(11);
    ASSERT_TRUE(scene.render().finished);
    scene.at(13);
    ASSERT_TRUE(scene.render().finished);
    EXPECT_GT(scene.z13Requests(), first) << "the tile missing offline was taken for a tile with no data";
}

// Online, a still waits for a slow tile, as before.
TEST(StillImageOffline, AnOnlineStillWaitsForASlowTile) {
    util::RunLoop loop;
    auto network = demCacheAndNetwork();
    network->latency = Milliseconds(500);
    DEMScene scene(network, MapMode::Static);
    const Still still = scene.render();
    ASSERT_TRUE(still.finished);
    ASSERT_FALSE(still.error) << message(still.error);
    ASSERT_TRUE(scene.elevation());
    EXPECT_NEAR(*scene.elevation(), tileMeters, 1.0);
    EXPECT_EQ(network->offlineAnswers, 0);
}

// Online, a Connection error still fails a still, as before.
TEST(StillImageOffline, AConnectionErrorOnlineFailsTheStill) {
    util::RunLoop loop;
    auto network = demCacheAndNetwork();
    network->server = [](const Resource& resource) -> std::optional<Response> {
        if (!isDEMTile(resource, 13)) return std::nullopt;
        Response res;
        res.error = std::make_unique<Response::Error>(Response::Error::Reason::Connection, "unreachable");
        return res;
    };
    DEMScene scene(network, MapMode::Static);
    const Still still = scene.render();
    ASSERT_TRUE(still.finished);
    EXPECT_TRUE(still.error) << "a Connection error online was taken for an offline network";
}

// Offline, a request that fails with a Connection error (in flight when the network went away)
// doesn't fail a still either: the still finishes over the ancestor.
TEST(StillImageOffline, AConnectionErrorOfflineDoesNotFailTheStill) {
    util::RunLoop loop;
    auto network = demCacheAndNetwork();
    network->failHeldRequests = true;
    DEMScene scene(network, MapMode::Static);
    OfflineNetwork offline;
    const Still still = scene.render();
    ASSERT_TRUE(still.finished);
    ASSERT_FALSE(still.error) << message(still.error);
    ASSERT_TRUE(scene.elevation());
    EXPECT_NEAR(*scene.elevation(), ancestorMeters, 1.0);
}

// A continuous map keeps today's behaviour offline: its request is held without an answer, it draws
// the ancestor meanwhile, and the tile arrives when the network is back.
TEST(StillImageOffline, AContinuousMapHoldsTheRequestAndDrawsTheAncestor) {
    util::RunLoop loop;
    FixtureLog log;
    auto network = demCacheAndNetwork();
    DEMScene scene(network, MapMode::Continuous);
    {
        OfflineNetwork offline;
        ASSERT_TRUE(scene.runUntil(
            [&] {
                const auto e = scene.elevation();
                return e && std::abs(*e - ancestorMeters) < 1.0 && scene.z13Requests() > 0;
            },
            std::chrono::seconds(10)))
            << "the continuous map did not draw the ancestor offline";
        EXPECT_EQ(network->offlineAnswers, 0) << "a continuous map's request asked to hear that the network is offline";
        EXPECT_GT(network->heldRequests(), 0u);
        EXPECT_EQ(offlineTileErrors(log), "");
    }
    ASSERT_TRUE(scene.runUntil(
        [&] {
            const auto e = scene.elevation();
            return e && std::abs(*e - tileMeters) < 1.0;
        },
        std::chrono::seconds(10)))
        << "the z13 tile did not arrive when the network was back";
}

// A continuous map that hears of the offline network by a Connection error keeps today's
// behaviour too: the tile fails to load.
TEST(StillImageOffline, AContinuousMapStillReportsAConnectionErrorOffline) {
    util::RunLoop loop;
    FixtureLog log;
    auto network = demCacheAndNetwork();
    network->failHeldRequests = true;
    DEMScene scene(network, MapMode::Continuous);
    OfflineNetwork offline;
    // unchecked() hands each message out once.
    bool failed = false;
    ASSERT_TRUE(scene.runUntil(
        [&] {
            failed = failed || !offlineTileErrors(log).empty();
            return failed;
        },
        std::chrono::seconds(10)))
        << "a continuous map took a Connection error offline for a tile with no data";
}
