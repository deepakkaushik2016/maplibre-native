#include <benchmark/benchmark.h>

#include <mln/gfx/context.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/gfx/renderer_backend.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map.hpp>
#include <mln/map/map_impl.hpp>
#include <mln/map/map_options.hpp>
#include <mln/map/transform.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/storage/file_source.hpp>
#include <mln/storage/network_status.hpp>
#include <mln/storage/resource.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/storage/response.hpp>
#include <mln/style/image.hpp>
#include <mln/style/style.hpp>
#include <mln/util/async_request.hpp>
#include <mln/util/client_options.hpp>
#include <mln/util/geo.hpp>
#include <mln/util/image.hpp>
#include <mln/util/io.hpp>
#include <mln/util/projection.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/tile_cover.hpp>

#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace mln;

// Terrain hot paths: a frame, rendered-feature queries, elevation queries, a DEM tile arriving,
// the camera clamp, and the elevation-aware tile cover. Everything is served offline: the DEM is a
// synthetic mountain range (Terrain-RGB, encoded on first request and cached for the process), and
// the vector content is inline GeoJSON. Run from the repository root (glyphs come from
// test/fixtures/resources/glyphs.pbf).

namespace {

constexpr uint32_t viewSize = 1000;
constexpr double exaggeration = 1.5;
constexpr double viewZoom = 13.0;
constexpr double viewPitch = 60.0;
constexpr double viewBearing = 30.0;
const LatLng sceneCenter{46.55, 7.98};

// The terrain, in metres: two crossed ridge systems a few kilometres apart, from about 100 m to
// 2,700 m, so any view at z13 holds well over 1,000 m of relief.
double groundMeters(double lng, double lat) {
    return 1400.0 + 800.0 * std::sin(lng * 120.0) * std::cos(lat * 150.0) + 450.0 * std::sin(lng * 47.0 + lat * 61.0);
}

// A 512 px Terrain-RGB tile (Mapbox encoding) of groundMeters. 512 px tiles put the DEM at the
// zoom of the vector tiles it lifts, as Mapbox and Mapterhorn serve it.
constexpr uint32_t demTileSize = 512;
std::string demTile(int z, int x, int y) {
    constexpr uint32_t dim = demTileSize;
    PremultipliedImage image({dim, dim});
    const double tiles = std::pow(2.0, z);
    std::vector<double> lngs(dim);
    for (uint32_t i = 0; i < dim; ++i) {
        lngs[i] = (x + (i + 0.5) / dim) / tiles * 360.0 - 180.0;
    }
    for (uint32_t j = 0; j < dim; ++j) {
        const double n = std::numbers::pi * (1.0 - 2.0 * (y + (j + 0.5) / dim) / tiles);
        const double lat = std::atan(std::sinh(n)) * 180.0 / std::numbers::pi;
        for (uint32_t i = 0; i < dim; ++i) {
            const auto value = static_cast<uint32_t>(std::lround((groundMeters(lngs[i], lat) + 10000.0) * 10.0));
            uint8_t* px = image.data.get() + (static_cast<size_t>(j) * dim + i) * 4;
            px[0] = static_cast<uint8_t>(value >> 16);
            px[1] = static_cast<uint8_t>((value >> 8) & 0xff);
            px[2] = static_cast<uint8_t>(value & 0xff);
            px[3] = 255;
        }
    }
    return encodePNG(image);
}

std::shared_ptr<const std::string> cachedDemTile(int z, int x, int y) {
    static std::map<std::tuple<int, int, int>, std::shared_ptr<const std::string>> cache;
    auto& entry = cache[{z, x, y}];
    if (!entry) {
        entry = std::make_shared<const std::string>(demTile(z, x, y));
    }
    return entry;
}

std::shared_ptr<const std::string> glyphs() {
    static const auto data = std::make_shared<const std::string>(util::read_file("test/fixtures/resources/glyphs.pbf"));
    return data;
}

// Serves the DEM and glyphs from memory, asynchronously on the run loop as a real file source
// does. One DEM tile can be held back and released later (Terrain_DemTileUpdate).
class TerrainFileSource final : public FileSource {
public:
    std::unique_ptr<AsyncRequest> request(const Resource& resource, Callback callback) override {
        Response response;
        if (resource.kind == Resource::Kind::Glyphs) {
            response.data = glyphs();
        } else if (resource.kind == Resource::Kind::Tile && resource.tileData) {
            const auto& t = *resource.tileData;
            requestedTiles.emplace_back(
                static_cast<uint8_t>(t.z), static_cast<uint32_t>(t.x), static_cast<uint32_t>(t.y));
            response.data = cachedDemTile(t.z, t.x, t.y);
            if (held && *held == requestedTiles.back()) {
                heldCallback = std::move(callback);
                heldResponse = std::move(response);
                return std::make_unique<HeldRequest>(*this);
            }
        } else {
            response.noContent = true;
        }
        auto guard = std::make_shared<Pending>(pending);
        return util::RunLoop::Get()->invokeCancellable(
            [guard, callback = std::move(callback), response = std::move(response)] { callback(response); });
    }

    bool canRequest(const Resource&) const override { return true; }
    void setResourceOptions(ResourceOptions options) override { resourceOptions = std::move(options); }
    ResourceOptions getResourceOptions() override { return resourceOptions.clone(); }
    void setClientOptions(ClientOptions options) override { clientOptions = std::move(options); }
    ClientOptions getClientOptions() override { return clientOptions.clone(); }

    /// Responses posted to the run loop and not yet delivered (or cancelled).
    int pendingResponses() const { return *pending; }

    /// Holds back the response to this DEM tile until release().
    std::optional<CanonicalTileID> held;
    bool isHeld() const { return static_cast<bool>(heldCallback); }
    void release() {
        auto callback = std::move(heldCallback);
        heldCallback = nullptr;
        if (callback) {
            callback(heldResponse);
        }
    }

    std::vector<CanonicalTileID> requestedTiles;

private:
    struct Pending {
        explicit Pending(std::shared_ptr<int> count_)
            : count(std::move(count_)) {
            ++*count;
        }
        ~Pending() { --*count; }
        Pending(const Pending&) = delete;
        Pending& operator=(const Pending&) = delete;
        std::shared_ptr<int> count;
    };

    class HeldRequest final : public AsyncRequest {
    public:
        explicit HeldRequest(TerrainFileSource& source_)
            : source(source_) {}
        ~HeldRequest() override { source.heldCallback = nullptr; }

    private:
        TerrainFileSource& source;
    };

    std::shared_ptr<int> pending = std::make_shared<int>(0);
    Callback heldCallback;
    Response heldResponse;
    ResourceOptions resourceOptions = ResourceOptions::Default();
    ClientOptions clientOptions;
};

// mln::Map with a file source of our own (as test/src/mln/test/map_adapter.hpp).
class TerrainMap : public Map {
public:
    TerrainMap(RendererFrontend& frontend, std::shared_ptr<FileSource> fileSource, const MapOptions& options)
        : Map(std::make_unique<Map::Impl>(frontend, MapObserver::nullObserver(), std::move(fileSource), options)) {}
};

// Inline GeoJSON over the view: `trails` wavy east-west lines of 64 vertices, and a grid of
// `peaks` named points.
std::string trailsGeoJSON(int trails) {
    std::ostringstream out;
    out.precision(9);
    out << R"({"type":"FeatureCollection","features":[)";
    for (int t = 0; t < trails; ++t) {
        const double lat = sceneCenter.latitude() - 0.06 + 0.12 * (t + 0.5) / trails;
        out << (t ? "," : "") << R"({"type":"Feature","properties":{"name":"trail )" << t
            << R"("},"geometry":{"type":"LineString","coordinates":[)";
        for (int v = 0; v < 64; ++v) {
            const double lng = sceneCenter.longitude() - 0.09 + 0.18 * v / 63.0;
            out << (v ? "," : "") << "[" << lng << "," << lat + 0.002 * std::sin(v * 0.7 + t) << "]";
        }
        out << "]}}";
    }
    out << "]}";
    return out.str();
}

std::string peaksGeoJSON(int side) {
    std::ostringstream out;
    out.precision(9);
    out << R"({"type":"FeatureCollection","features":[)";
    for (int j = 0; j < side; ++j) {
        for (int i = 0; i < side; ++i) {
            const double lat = sceneCenter.latitude() - 0.06 + 0.12 * (j + 0.5) / side;
            const double lng = sceneCenter.longitude() - 0.09 + 0.18 * (i + 0.5) / side;
            out << ((i || j) ? "," : "") << R"({"type":"Feature","properties":{"name":"Peak )" << (j * side + i)
                << R"("},"geometry":{"type":"Point","coordinates":[)" << lng << "," << lat << "]}}";
        }
    }
    out << "]}";
    return out.str();
}

struct SceneOptions {
    bool terrain = true;
    /// Points per side of the symbol grid (side * side symbols, each an icon and a label).
    int peaksPerSide = 20;
    MapMode mode = MapMode::Static;
};

std::string terrainStyle(const SceneOptions& options) {
    std::ostringstream out;
    out << R"({
  "version": 8,
  "glyphs": "http://example.com/fonts/{fontstack}/{range}.pbf",
  "sources": {
    "dem": {"type": "raster-dem", "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"], "encoding": "mapbox",
            "minzoom": 11, "maxzoom": 14, "tileSize": 512},
    "trails": {"type": "geojson", "data": )"
        << trailsGeoJSON(30) << R"(},
    "peaks": {"type": "geojson", "data": )"
        << peaksGeoJSON(options.peaksPerSide) << R"(}
  },)";
    if (options.terrain) {
        out << R"("terrain": {"source": "dem", "exaggeration": )" << exaggeration << "},";
    }
    out << R"(
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "#e8e4d8"}},
    {"id": "trails", "type": "line", "source": "trails", "paint": {"line-color": "#c03020", "line-width": 4}},
    {"id": "peaks", "type": "symbol", "source": "peaks",
     "layout": {"icon-image": "marker", "text-field": ["get", "name"], "text-font": ["Open Sans Regular"],
                "text-size": 12, "text-offset": [0, 1.2], "text-anchor": "top"},
     "paint": {"text-color": "#202020"}}
  ]
})";
    return out.str();
}

PremultipliedImage marker() {
    constexpr uint32_t size = 12;
    PremultipliedImage image({size, size});
    for (uint32_t i = 0; i < size * size; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        px[0] = 200;
        px[1] = 40;
        px[2] = 40;
        px[3] = 255;
    }
    return image;
}

CameraOptions sceneCamera(const LatLng& center = sceneCenter) {
    // The centre sits on the drawn ground, as gl-js keeps it (and as the clamp does); the same
    // camera is used without terrain so both frames see the same view.
    return CameraOptions()
        .withCenter(center)
        .withZoom(viewZoom)
        .withPitch(viewPitch)
        .withBearing(viewBearing)
        .withCenterAltitude(groundMeters(center.longitude(), center.latitude()) * exaggeration);
}

struct TerrainScene {
    explicit TerrainScene(const SceneOptions& options_, bool invalidateOnUpdate = true)
        : options(options_),
          frontend({viewSize, viewSize},
                   1,
                   gfx::HeadlessBackend::SwapBehaviour::NoFlush,
                   gfx::ContextMode::Unique,
                   std::nullopt,
                   invalidateOnUpdate),
          map(frontend, fileSource, MapOptions().withMapMode(options.mode).withSize(frontend.getSize())) {
        NetworkStatus::Set(NetworkStatus::Status::Offline);
    }

    void load() {
        map.getStyle().loadJSON(terrainStyle(options));
        map.getStyle().addImage(std::make_unique<style::Image>("marker", marker(), 1.0f));
        map.jumpTo(sceneCamera());
    }

    /// Static mode: one still render, which waits for every resource and parse.
    PremultipliedImage renderStill() { return frontend.render(map).image; }

    /// Continuous mode: runs the loop and renders frames until no response is in flight and the
    /// map reports itself loaded (or `timeout` passes, e.g. while a tile is held back).
    void settle(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        int quiet = 0;
        while (std::chrono::steady_clock::now() < deadline && quiet < 20) {
            loop.runOnce();
            frontend.renderFrame();
            quiet = (fileSource->pendingResponses() == 0 && map.isFullyLoaded()) ? quiet + 1 : 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    SceneOptions options;
    util::RunLoop loop;
    std::shared_ptr<TerrainFileSource> fileSource = std::make_shared<TerrainFileSource>();
    HeadlessFrontend frontend;
    TerrainMap map;
};

RenderedQueryOptions queryLayers() {
    RenderedQueryOptions options;
    options.layerIDs = std::vector<std::string>{"trails", "peaks"};
    return options;
}

// A screen point over the relief that hits a draped trail: the grid point nearest the screen
// centre that a tap there finds a feature at.
ScreenCoordinate tapPoint(TerrainScene& scene) {
    std::optional<ScreenCoordinate> best;
    double bestDistance = 0;
    for (int y = 300; y <= 700; y += 4) {
        for (int x = 300; x <= 700; x += 4) {
            const ScreenCoordinate p{static_cast<double>(x), static_cast<double>(y)};
            const double d = std::hypot(p.x - viewSize / 2.0, p.y - viewSize / 2.0);
            if (best && d >= bestDistance) continue;
            RenderedQueryOptions trails;
            trails.layerIDs = std::vector<std::string>{"trails"};
            if (!scene.frontend.getRenderer()->queryRenderedFeatures(p, trails).empty()) {
                best = p;
                bestDistance = d;
            }
        }
    }
    return best.value_or(ScreenCoordinate{viewSize / 2.0, viewSize / 2.0});
}

void addFrameCounters(::benchmark::State& state, const gfx::RenderingStats& stats) {
    state.counters["drawCalls"] = stats.numDrawCalls;
}

// 1. One still frame at z13, pitch 60, exaggeration 1.5, 1000x1000, terrain on (arg 1) or off
//    (arg 0, the same style without "terrain"). Includes placement and the frame's read-back, as
//    API_renderStill_* do.
void Terrain_RenderFrame(::benchmark::State& state) {
    TerrainScene scene({.terrain = state.range(0) != 0});
    scene.load();
    scene.renderStill();
    gfx::RenderingStats stats;
    for (auto _ : state) {
        stats = scene.frontend.render(scene.map).stats;
    }
    addFrameCounters(state, stats);
}

// The same frame with a symbol-heavy style (60x60 = 3,600 icons with labels), where placement
// dominates: what terrain adds to placement (each symbol's collision boxes lifted by the DEM).
void Terrain_RenderFrameSymbols(::benchmark::State& state) {
    TerrainScene scene({.terrain = state.range(0) != 0, .peaksPerSide = 60});
    scene.load();
    scene.renderStill();
    gfx::RenderingStats stats;
    for (auto _ : state) {
        stats = scene.frontend.render(scene.map).stats;
    }
    addFrameCounters(state, stats);
}

// 2. queryRenderedFeatures at one point over the relief (draped line layer plus symbol layer).
void Terrain_QueryTap(::benchmark::State& state) {
    TerrainScene scene({.terrain = state.range(0) != 0});
    scene.load();
    scene.renderStill();
    const ScreenCoordinate point = tapPoint(scene);
    const auto options = queryLayers();
    size_t hits = 0;
    for (auto _ : state) {
        hits = scene.frontend.getRenderer()->queryRenderedFeatures(point, options).size();
        ::benchmark::DoNotOptimize(hits);
    }
    state.counters["hits"] = static_cast<double>(hits);
}

// 3. A 200x200 px box over the relief.
void Terrain_QueryBox(::benchmark::State& state) {
    TerrainScene scene({.terrain = state.range(0) != 0});
    scene.load();
    scene.renderStill();
    const ScreenBox box{{400, 400}, {600, 600}};
    const auto options = queryLayers();
    size_t hits = 0;
    for (auto _ : state) {
        hits = scene.frontend.getRenderer()->queryRenderedFeatures(box, options).size();
        ::benchmark::DoNotOptimize(hits);
    }
    state.counters["hits"] = static_cast<double>(hits);
}

// 3. A 64-point lasso (a closed ring of radius 120 px) over the relief.
void Terrain_QueryLasso(::benchmark::State& state) {
    TerrainScene scene({.terrain = state.range(0) != 0});
    scene.load();
    scene.renderStill();
    ScreenLineString lasso;
    for (int i = 0; i < 64; ++i) {
        const double a = 2.0 * std::numbers::pi * i / 63.0;
        lasso.emplace_back(500.0 + 120.0 * std::cos(a), 500.0 + 120.0 * std::sin(a));
    }
    const auto options = queryLayers();
    size_t hits = 0;
    for (auto _ : state) {
        hits = scene.frontend.getRenderer()->queryRenderedFeatures(lasso, options).size();
        ::benchmark::DoNotOptimize(hits);
    }
    state.counters["hits"] = static_cast<double>(hits);
}

// 4. Renderer::queryTerrainElevation at 1,000 points along a 10 km profile (the elevation-profile
//    use).
void Terrain_QueryElevation(::benchmark::State& state) {
    TerrainScene scene({.terrain = true});
    scene.load();
    scene.renderStill();
    std::vector<LatLng> profile;
    profile.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        const double t = i / 999.0;
        profile.emplace_back(sceneCenter.latitude() - 0.02 + 0.04 * t, sceneCenter.longitude() - 0.06 + 0.12 * t);
    }
    const auto* renderer = scene.frontend.getRenderer();
    size_t found = 0;
    for (auto _ : state) {
        found = 0;
        for (const auto& point : profile) {
            found += renderer->queryTerrainElevation(point) ? 1 : 0;
        }
        ::benchmark::DoNotOptimize(found);
    }
    state.counters["found"] = static_cast<double>(found);
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * 1000);
}

// 5. A DEM tile arriving: Continuous mode, every tile loaded but the finest DEM tile under the
//    centre; the time from its response to the first frame that samples it (worker decode and
//    backfill, then the frame that uploads its texture and rebinds the mesh tiles over it).
//    Counter: that frame's CPU time.
void Terrain_DemTileUpdate(::benchmark::State& state) {
    // Which DEM tile under the centre is finest: ask a throwaway map.
    std::optional<CanonicalTileID> target;
    {
        TerrainScene probe({.terrain = true, .mode = MapMode::Continuous});
        probe.map.setPrefetchZoomDelta(0);
        probe.load();
        probe.settle();
        const auto world = Projection::project(sceneCenter, 1.0);
        for (const auto& id : probe.fileSource->requestedTiles) {
            const double tiles = std::pow(2.0, id.z);
            const bool under = static_cast<uint32_t>(world.x / util::tileSize_D * tiles) == id.x &&
                               static_cast<uint32_t>(world.y / util::tileSize_D * tiles) == id.y;
            if (under && (!target || id.z > target->z)) target = id;
        }
    }
    if (!target) {
        state.SkipWithError("no DEM tile under the centre was requested");
        return;
    }
    double frameMs = 0;
    for (auto _ : state) {
        state.PauseTiming();
        auto scene = std::make_unique<TerrainScene>(SceneOptions{.terrain = true, .mode = MapMode::Continuous});
        scene->map.setPrefetchZoomDelta(0);
        scene->fileSource->held = target;
        scene->load();
        scene->settle(std::chrono::milliseconds(1500));
        if (!scene->fileSource->isHeld()) {
            state.SkipWithError("the DEM tile under the centre was not requested");
            break;
        }
        const auto before = scene->frontend.getRenderer()->queryTerrainElevation(sceneCenter);
        state.ResumeTiming();

        const auto start = std::chrono::steady_clock::now();
        scene->fileSource->release();
        while (scene->frontend.getRenderer()->queryTerrainElevation(sceneCenter) == before) {
            scene->loop.runOnce();
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
                state.SkipWithError("the released DEM tile never reached a frame");
                break;
            }
        }
        state.PauseTiming();
        frameMs += scene->frontend.getFrameTime() * 1e3;
        scene.reset();
        state.ResumeTiming();
    }
    if (state.iterations() > 0) {
        state.counters["arrivalFrame_ms"] = frameMs / static_cast<double>(state.iterations());
    }
}

// 6. Per-frame cost of the centre clamp and eye-clearance reporting: Continuous frames while the
//    centre circles 400 m round the scene centre over the relief. Arg 0: terrain off; 1: terrain on,
//    clamp off; 2: terrain on, clamp on (Map::setCenterClampedToGround).
//    Arg 2 minus arg 1 is not the clamp's own cost (one elevation sample per frame): clamping
//    moves the centre altitude with the ground, so more drape targets re-render each frame (see
//    the drapesRendered/frame counter), and the delta is mostly those extra drape re-renders.
void Terrain_CameraClampFrame(::benchmark::State& state) {
    const bool terrain = state.range(0) != 0;
    TerrainScene scene({.terrain = terrain, .mode = MapMode::Continuous}, /*invalidateOnUpdate=*/false);
    scene.map.setCenterClampedToGround(state.range(0) == 2);
    scene.load();
    constexpr int steps = 64;
    std::vector<LatLng> path;
    for (int i = 0; i < steps; ++i) {
        const double a = 2.0 * std::numbers::pi * i / steps;
        path.emplace_back(sceneCenter.latitude() + 0.0036 * std::sin(a),
                          sceneCenter.longitude() + 0.0052 * std::cos(a));
    }
    // Load every tile the path sees, then measure frames that load nothing new.
    for (const auto& center : path) {
        scene.map.jumpTo(CameraOptions().withCenter(center));
        scene.settle(std::chrono::milliseconds(200));
    }
    const auto& stats = scene.frontend.getBackend()->getContext().renderingStats();
    const int drapesBefore = stats.numDrapeTargetsRendered;
    const int uniformUpdatesBefore = stats.numUniformUpdates;
    size_t i = 0;
    for (auto _ : state) {
        scene.map.jumpTo(CameraOptions().withCenter(path[i++ % steps]));
        scene.loop.runOnce();
        scene.frontend.renderFrame();
    }
    addFrameCounters(state, stats);
    const auto frames = static_cast<double>(state.iterations());
    state.counters["drapesRendered/frame"] = (stats.numDrapeTargetsRendered - drapesBefore) / frames;
    state.counters["uniformUpdates/frame"] = (stats.numUniformUpdates - uniformUpdatesBefore) / frames;
    state.counters["centerAltitude_m"] = scene.map.getCameraOptions().centerAltitude.value_or(0.0);
}

// An elevation provider over the synthetic terrain: every tile spans the terrain's full range, as a
// coarse DEM-backed provider would report it.
class SyntheticElevation final : public util::TileElevationProvider {
public:
    std::optional<Range<double>> getTileElevationRange(const CanonicalTileID&) const override {
        return Range<double>{100.0 * exaggeration, 2650.0 * exaggeration};
    }
};

void tileCoverWithTerrain(::benchmark::State& state, double pitch) {
    Transform transform;
    transform.resize({viewSize, viewSize});
    transform.setMaxPitch(85.0);
    transform.jumpTo(sceneCamera().withPitch(pitch));
    const SyntheticElevation elevation;
    util::TileCoverParameters parameters{.transformState = transform.getState()};
    if (state.range(0) != 0) {
        parameters.elevationProvider = &elevation;
    }
    size_t tiles = 0;
    for (auto _ : state) {
        tiles = util::tileCover(parameters, static_cast<uint8_t>(viewZoom), {0, 14}).size();
        ::benchmark::DoNotOptimize(tiles);
    }
    state.counters["tiles"] = static_cast<double>(tiles);
}

// util::tileCover of the scene's view (z13, pitch 60, centre on the ground), with the synthetic
// elevation provider (arg 1) or flat (arg 0).
void Terrain_TileCover(::benchmark::State& state) {
    tileCoverWithTerrain(state, viewPitch);
}

// The same at pitch 80, where the cover reaches far past the centre.
void Terrain_TileCoverSteep(::benchmark::State& state) {
    tileCoverWithTerrain(state, 80.0);
}

BENCHMARK(Terrain_RenderFrame)->Arg(0)->Arg(1)->Unit(::benchmark::kMillisecond)->Iterations(40);
BENCHMARK(Terrain_RenderFrameSymbols)->Arg(0)->Arg(1)->Unit(::benchmark::kMillisecond)->Iterations(20);
BENCHMARK(Terrain_QueryTap)->Arg(0)->Arg(1)->Unit(::benchmark::kMicrosecond);
BENCHMARK(Terrain_QueryBox)->Arg(0)->Arg(1)->Unit(::benchmark::kMicrosecond);
BENCHMARK(Terrain_QueryLasso)->Arg(0)->Arg(1)->Unit(::benchmark::kMicrosecond);
BENCHMARK(Terrain_QueryElevation)->Unit(::benchmark::kMicrosecond);
BENCHMARK(Terrain_DemTileUpdate)->UseRealTime()->Unit(::benchmark::kMillisecond)->Iterations(10);
BENCHMARK(Terrain_CameraClampFrame)->Arg(0)->Arg(1)->Arg(2)->Unit(::benchmark::kMillisecond)->Iterations(1024);
BENCHMARK(Terrain_TileCover)->Arg(0)->Arg(1)->Unit(::benchmark::kMicrosecond);
BENCHMARK(Terrain_TileCoverSteep)->Arg(0)->Arg(1)->Unit(::benchmark::kMicrosecond);

} // namespace
