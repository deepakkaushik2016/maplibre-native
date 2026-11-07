#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/context.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/gfx/renderer_backend.hpp>
#include <mln/gfx/rendering_stats.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_observer.hpp>
#include <mln/map/map_options.hpp>
#include <mln/renderer/render_layer.hpp>
#include <mln/renderer/buckets/debug_bucket.hpp>
#include <mln/renderer/render_tile.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/renderer/source_state.hpp>
#include <mln/renderer/tile_render_data.hpp>
#include <mln/storage/response.hpp>
#include <mln/style/conversion/geojson.hpp>
#include <mln/style/conversion/json.hpp>
#include <mln/style/conversion/property_value.hpp>
#include <mln/style/expression/error.hpp>
#include <mln/style/image.hpp>
#include <mln/style/layer.hpp>
#include <mln/style/layers/line_layer.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/style/style.hpp>
#include <mln/tile/tile.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/geojson.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace mln;

// Feature state and image updates must cost nothing at rest, and a change must re-upload and
// re-bake only what it touches. Selection and hover through feature state are the app's headline
// interaction: once any state was set on a source, every tile of every layer of that source
// re-uploaded its vertex buffers on every frame, and one change re-baked every drape target the
// source covered. An in-place image update re-baked every draped pattern layer, whatever image it
// draws.
//
// Uploads are counted with RenderingStats::vertexUpdateBytes (the bytes written to existing vertex
// buffers) and drape bakes with RenderingStats::numDrapeTargetsRendered.

namespace {

constexpr uint32_t viewSize = 1024;
constexpr int demZoom = 10;
constexpr uint32_t demSize = 256;
constexpr int tileZoom = 13;
constexpr double kmPerDegree = 6371.0088 * M_PI / 180.0;
constexpr double alpsLat = 47.265;
constexpr double alpsLng = 11.404;
// Draws per scene after a change, at rest, that must upload and bake nothing.
constexpr int restFrameCount = 8;

double ridgeMeters(double lat, double lng) {
    const double northKm = (lat - alpsLat) * kmPerDegree;
    const double eastKm = (lng - alpsLng) * kmPerDegree * std::cos(util::deg2rad(alpsLat));
    const double ridge = std::exp(-std::pow(northKm / 4.0, 2.0));
    const double crest = 0.9 + 0.1 * std::cos(2.0 * M_PI * eastKm / 12.0);
    return 575.0 + (2300.0 - 575.0) * ridge * crest;
}

std::string ridgeTile(const CanonicalTileID& id) {
    const double n = std::pow(2.0, id.z);
    PremultipliedImage image({demSize, demSize});
    for (uint32_t row = 0; row < demSize; ++row) {
        const double mercatorY = (id.y + (row + 0.5) / demSize) / n;
        const double latitude = util::rad2deg(std::atan(std::sinh(M_PI * (1.0 - 2.0 * mercatorY))));
        for (uint32_t col = 0; col < demSize; ++col) {
            const double longitude = (id.x + (col + 0.5) / demSize) / n * 360.0 - 180.0;
            const double value = ridgeMeters(latitude, longitude) + 32768.0;
            const auto whole = static_cast<uint32_t>(std::floor(value));
            uint8_t* px = image.data.get() + (static_cast<size_t>(row) * demSize + col) * 4;
            px[0] = static_cast<uint8_t>(whole >> 8);
            px[1] = static_cast<uint8_t>(whole & 0xFF);
            px[2] = static_cast<uint8_t>(std::floor((value - whole) * 256.0));
            px[3] = 255;
        }
    }
    return encodePNG(image);
}

// The z13 tile holding the ridge's flank; the view is centred on it and shows it whole, with half
// of each of its eight neighbours.
const CanonicalTileID centreTile = [] {
    const double n = std::pow(2.0, tileZoom);
    const double lat = alpsLat + 5.0 / kmPerDegree;
    const double x = (alpsLng + 180.0) / 360.0 * n;
    const double y = (1.0 - std::asinh(std::tan(util::deg2rad(lat))) / M_PI) / 2.0 * n;
    return CanonicalTileID(tileZoom, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
}();

LatLng tilePoint(double x, double y) {
    const double n = std::pow(2.0, tileZoom);
    const double lng = x / n * 360.0 - 180.0;
    const double lat = util::rad2deg(std::atan(std::sinh(M_PI * (1.0 - 2.0 * y / n))));
    return {lat, lng};
}

// One trail per tile of the 3×3 block around the centre tile, wholly inside its tile, numbered
// 1..9 row by row; the centre tile's is 5.
int featureID(int dx, int dy) {
    return 1 + (dx + 1) + 3 * (dy + 1);
}
constexpr int centreFeature = 5;
// The line in tile-local coordinates (0..1), the same in every tile.
const std::vector<std::pair<double, double>> trailShape = {{0.25, 0.5}, {0.5, 0.45}, {0.75, 0.5}};

// --- A minimal Mapbox Vector Tile encoder (one layer of line strings with ids). ---
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

std::string vectorTile(const CanonicalTileID& id) {
    constexpr int extent = 4096;
    std::string layer;
    fieldKey(layer, 15, 0);
    varint(layer, 2);
    bytesField(layer, 1, "trails");
    const int dx = static_cast<int>(id.x) - static_cast<int>(centreTile.x);
    const int dy = static_cast<int>(id.y) - static_cast<int>(centreTile.y);
    if (id.z == tileZoom && std::abs(dx) <= 1 && std::abs(dy) <= 1) {
        std::string feature;
        fieldKey(feature, 1, 0);
        varint(feature, static_cast<uint64_t>(featureID(dx, dy)));
        fieldKey(feature, 3, 0);
        varint(feature, 2); // LINESTRING
        std::string geometry;
        int cx = 0;
        int cy = 0;
        for (std::size_t i = 0; i < trailShape.size(); ++i) {
            if (i == 0) {
                varint(geometry, 1 | (1 << 3)); // MoveTo, 1
            } else if (i == 1) {
                varint(geometry, 2 | ((trailShape.size() - 1) << 3)); // LineTo, n - 1
            }
            const int x = static_cast<int>(trailShape[i].first * extent);
            const int y = static_cast<int>(trailShape[i].second * extent);
            varint(geometry, zigzag(x - cx));
            varint(geometry, zigzag(y - cy));
            cx = x;
            cy = y;
        }
        bytesField(feature, 4, geometry);
        bytesField(layer, 2, feature);
    }
    fieldKey(layer, 5, 0);
    varint(layer, extent);
    std::string tile;
    bytesField(tile, 3, layer);
    return tile;
}

std::string lonLat(const LatLng& p) {
    return "[" + std::to_string(p.longitude()) + ", " + std::to_string(p.latitude()) + "]";
}

// The same trails as GeoJSON, plus, for the pattern tests, an area inside the centre tile (far
// enough from its edges to stay out of its neighbours' buffers) with a "kind" and a "pattern"
// property.
std::string trailsGeoJSON() {
    std::string features;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            std::string coordinates;
            for (const auto& [fx, fy] : trailShape) {
                coordinates += (coordinates.empty() ? "" : ", ") +
                               lonLat(tilePoint(centreTile.x + dx + fx, centreTile.y + dy + fy));
            }
            features += (features.empty() ? "" : ",\n") + std::string(R"({"type": "Feature", "id": )") +
                        std::to_string(featureID(dx, dy)) +
                        R"(, "properties": {}, "geometry": {"type": "LineString", "coordinates": [)" + coordinates +
                        "]}}";
        }
    }
    const auto corner = [](double fx, double fy) {
        return lonLat(tilePoint(centreTile.x + fx, centreTile.y + fy));
    };
    features += R"(,
      {"type": "Feature", "id": 100, "properties": {"kind": "a", "pattern": "stripe"},
       "geometry": {"type": "Polygon", "coordinates": [[)" +
                corner(0.3, 0.55) + ", " + corner(0.7, 0.55) + ", " + corner(0.7, 0.7) + ", " + corner(0.3, 0.7) +
                ", " + corner(0.3, 0.55) + "]]}}";
    return R"({"type": "FeatureCollection", "features": [)" + features + "]}";
}

// Per tile of the 3x3 block, a square area and a point, both with the tile's feature id (1..9): a
// fill, a fill-extrusion and a symbol layer each draw one of them. The square stays out of the
// neighbours' tile buffers; the point sits towards the view's centre, so the half-shown
// neighbours' icons are in view.
std::string shapesGeoJSON() {
    std::string features;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const auto at = [&](double fx, double fy) {
                return lonLat(tilePoint(centreTile.x + dx + fx, centreTile.y + dy + fy));
            };
            const std::string id = std::to_string(featureID(dx, dy));
            features += (features.empty() ? "" : ",\n") + std::string(R"({"type": "Feature", "id": )") + id +
                        R"(, "properties": {}, "geometry": {"type": "Polygon", "coordinates": [[)" + at(0.3, 0.3) +
                        ", " + at(0.7, 0.3) + ", " + at(0.7, 0.7) + ", " + at(0.3, 0.7) + ", " + at(0.3, 0.3) +
                        R"(]]}},
      {"type": "Feature", "id": )" +
                        id + R"(, "properties": {}, "geometry": {"type": "Point", "coordinates": )" +
                        at(0.5 - 0.3 * dx, 0.5 - 0.3 * dy) + "}}";
        }
    }
    return R"({"type": "FeatureCollection", "features": [)" + features + "]}";
}

std::string style(const std::string& layers) {
    return R"STYLE({
  "version": 8,
  "transition": {"duration": 0, "delay": 0},
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"],
      "encoding": "terrarium",
      "minzoom": )STYLE" +
           std::to_string(demZoom) + R"STYLE(, "maxzoom": )STYLE" + std::to_string(demZoom) + R"STYLE(,
      "tileSize": 256
    },
    "vt": {
      "type": "vector",
      "tiles": ["http://example.com/vt/{z}-{x}-{y}.pbf"],
      "minzoom": )STYLE" +
           std::to_string(tileZoom) + R"STYLE(, "maxzoom": )STYLE" + std::to_string(tileZoom) + R"STYLE(
    },
    "overlay": {"type": "geojson", "data": )STYLE" +
           trailsGeoJSON() + R"STYLE(},
    "shapes": {"type": "geojson", "data": )STYLE" +
           shapesGeoJSON() + R"STYLE(}
  },
  "terrain": {"source": "dem", "exaggeration": 1.25},
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "#dddddd"}},
    )STYLE" +
           layers + R"STYLE(
  ]
})STYLE";
}

// The selected-trail highlight driven by feature state, on either source: a colour (a source
// function) and a zoom-dependent width (a composite function).
std::string selectableTrails(const std::string& source) {
    const std::string sourceLayer = source == "vt" ? R"(, "source-layer": "trails")" : "";
    return R"({"id": "trail", "type": "line", "source": ")" + source + "\"" + sourceLayer +
           R"(, "filter": ["==", ["geometry-type"], "LineString"],
      "paint": {
        "line-color": ["case", ["boolean", ["feature-state", "selected"], false], "#2040e0", "#e02020"],
        "line-width": ["interpolate", ["linear"], ["zoom"],
          10, ["case", ["boolean", ["feature-state", "selected"], false], 7, 4],
          16, ["case", ["boolean", ["feature-state", "selected"], false], 28, 16]]}})";
}

// What selectableTrails draws with the given features selected, with no feature state.
std::string trailsSelectedByID(const std::string& source, const std::vector<int>& ids) {
    const std::string sourceLayer = source == "vt" ? R"(, "source-layer": "trails")" : "";
    std::string list;
    for (const int id : ids) {
        list += (list.empty() ? "" : ", ") + std::to_string(id);
    }
    const std::string selected = ids.empty() ? "false" : R"(["in", ["id"], ["literal", [)" + list + "]]]";
    return R"({"id": "trail", "type": "line", "source": ")" + source + "\"" + sourceLayer +
           R"(, "filter": ["==", ["geometry-type"], "LineString"],
      "paint": {
        "line-color": ["case", )" +
           selected + R"(, "#2040e0", "#e02020"],
        "line-width": ["interpolate", ["linear"], ["zoom"], 10, ["case", )" +
           selected + R"(, 7, 4], 16, ["case", )" + selected + R"(, 28, 16]]}})";
}

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
    std::set<std::pair<OverscaledTileID, std::string>> parsing;
    bool idleFrame = false;
};

struct Scene {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    HeadlessFrontend frontend{{viewSize, viewSize}, 1};
    FrameObserver observer;
    MapAdapter map{
        frontend, observer, fileSource, MapOptions().withMapMode(MapMode::Continuous).withSize(frontend.getSize())};

    /// `setup` runs on the style right after it is loaded; `beforeLoad` before the camera moves
    /// there, so before any tile of the view has loaded.
    explicit Scene(const std::string& layers,
                   const std::function<void(style::Style&)>& setup = {},
                   const std::function<void(Scene&)>& beforeLoad = {}) {
        fileSource->tileResponse = [](const Resource& resource) -> std::optional<Response> {
            Response res;
            if (!resource.tileData) {
                res.noContent = true;
                return res;
            }
            const CanonicalTileID id(
                resource.tileData->z, static_cast<uint32_t>(resource.tileData->x), resource.tileData->y);
            if (resource.url.find("/vt/") != std::string::npos) {
                res.data = std::make_shared<std::string>(vectorTile(id));
            } else if (resource.url.find("/dem/") != std::string::npos && id.z == demZoom) {
                res.data = std::make_shared<std::string>(ridgeTile(id));
            } else {
                res.error = std::make_unique<Response::Error>(Response::Error::Reason::NotFound, "no such tile");
            }
            return res;
        };
        map.getStyle().loadJSON(style(layers));
        if (setup) {
            setup(map.getStyle());
        }
        if (beforeLoad) {
            // Somewhere far away, so no tile of the view is loaded yet.
            map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(2.0));
            settle();
            beforeLoad(*this);
        }
        map.jumpTo(CameraOptions()
                       .withCenter(tilePoint(centreTile.x + 0.5, centreTile.y + 0.5))
                       .withZoom(static_cast<double>(tileZoom))
                       .withPitch(0.0)
                       .withBearing(0.0));
        settle();
    }

    // Runs the map until every tile has arrived, been parsed and drawn, and a frame has found
    // nothing more to draw (at most 30 s).
    void settle() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        const int start = observer.frames;
        observer.idleFrame = false;
        map.triggerRepaint();
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

    // Runs the loop until the next frame has been drawn (at most 10 s).
    bool nextFrame() {
        const int start = observer.frames;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (observer.frames == start && std::chrono::steady_clock::now() < deadline) {
            loop.runOnce();
        }
        return observer.frames != start;
    }

    void restFrames(int count) {
        for (int i = 0; i < count; ++i) {
            map.triggerRepaint();
            ASSERT_TRUE(nextFrame());
        }
    }

    // Applies `change` and draws one frame; returns what that frame uploaded and baked.
    struct Cost {
        std::size_t uploadBytes = 0;
        int bakes = 0;
    };
    Cost frameAfter(const std::function<void()>& change) {
        const auto bytes = uploadBytes();
        const auto baked = bakes();
        change();
        map.triggerRepaint();
        EXPECT_TRUE(nextFrame());
        return {.uploadBytes = uploadBytes() - bytes, .bakes = bakes() - baked};
    }
    // What `count` frames at rest uploaded and baked.
    Cost restCost(int count = restFrameCount) {
        const auto bytes = uploadBytes();
        const auto baked = bakes();
        restFrames(count);
        return {.uploadBytes = uploadBytes() - bytes, .bakes = bakes() - baked};
    }

    std::size_t uploadBytes() { return stats().vertexUpdateBytes; }
    int bakes() { return stats().numDrapeTargetsRendered; }
    gfx::RenderingStats& stats() { return frontend.getBackend()->getContext().renderingStats(); }

    Renderer& renderer() { return *frontend.getRenderer(); }

    PremultipliedImage image() {
        gfx::BackendScope guard{*frontend.getBackend()};
        return frontend.readStillImage();
    }
};

FeatureState selectedState(bool selected = true) {
    FeatureState state;
    state["selected"] = selected;
    return state;
}

std::optional<std::string> sourceLayerOf(const std::string& source) {
    return source == "vt" ? std::optional<std::string>("trails") : std::nullopt;
}

double differingShare(const PremultipliedImage& a, const PremultipliedImage& b) {
    if (a.size != b.size) {
        return 1.0;
    }
    size_t differing = 0;
    const size_t pixels = static_cast<size_t>(a.size.width) * a.size.height;
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t* pa = a.data.get() + i * 4;
        const uint8_t* pb = b.data.get() + i * 4;
        for (int c = 0; c < 4; ++c) {
            if (std::abs(pa[c] - pb[c]) > 4) {
                ++differing;
                break;
            }
        }
    }
    return static_cast<double>(differing) / static_cast<double>(pixels);
}
constexpr double maxMismatch = 0.002;
constexpr double minChange = 0.001;

using Selection = std::vector<int>;
using References = std::map<Selection, PremultipliedImage>;

// Frames of fresh scenes that draw each selection (and none) as an ["id"] paint with no feature
// state. Made before the scene under test: two maps alive at once on one thread share its current
// run loop, and the inner one's teardown leaves the outer one, and later tests, without it.
References references(const std::function<std::string(const Selection&)>& layers,
                      std::initializer_list<Selection> selections,
                      const std::function<void(style::Style&)>& setup = {}) {
    References out;
    out.emplace(Selection{}, Scene(layers({}), setup).image());
    for (const auto& ids : selections) {
        if (!out.contains(ids)) {
            out.emplace(ids, Scene(layers(ids), setup).image());
        }
    }
    return out;
}

References references(const std::string& source, std::initializer_list<Selection> selections) {
    return references([&](const Selection& ids) { return trailsSelectedByID(source, ids); }, selections);
}

// A frame of `scene` matches the fresh scene drawn with `ids` selected.
void expectShowsSelected(Scene& scene, const References& refs, const Selection& ids) {
    scene.settle();
    const auto& reference = refs.at(ids);
    if (!ids.empty()) {
        ASSERT_GE(differingShare(reference, refs.at({})), minChange) << "the selection does not show: no proof";
    }
    EXPECT_LE(differingShare(scene.image(), reference), maxMismatch) << "the frame does not show the selection";
}

class TerrainFeatureState : public ::testing::TestWithParam<std::string> {};

} // namespace

// R1(a): one feature's state on a source with several tiles. Once the change is drawn, the map at
// rest uploads no vertex and bakes no drape target.
TEST_P(TerrainFeatureState, AtRestNothingIsUploadedOrBaked) {
    const std::string source = GetParam();
    const auto refs = references(source, {{centreFeature}});
    Scene scene(selectableTrails(source));
    const auto change = scene.frameAfter(
        [&] { scene.renderer().setFeatureState(source, sourceLayerOf(source), "5", selectedState()); });
    EXPECT_GT(change.uploadBytes, 0u) << "the change uploaded nothing: the counter proves nothing";
    EXPECT_GT(change.bakes, 0) << "the change baked nothing";
    scene.settle();
    const auto rest = scene.restCost();
    RecordProperty("restUploadBytes", std::to_string(rest.uploadBytes));
    RecordProperty("restBakes", std::to_string(rest.bakes));
    EXPECT_EQ(rest.uploadBytes, 0u) << "vertex bytes uploaded over " << restFrameCount << " frames at rest";
    EXPECT_EQ(rest.bakes, 0) << "drape targets baked over " << restFrameCount << " frames at rest";
    expectShowsSelected(scene, refs, {centreFeature});
}

// R1(b): a change to one tile's feature uploads and bakes only what covers that feature. The
// tiles' buckets are alike (the same trail in each), so each tile's upload is the same size.
TEST_P(TerrainFeatureState, AChangeUploadsAndBakesOnlyItsTiles) {
    const std::string source = GetParam();
    const auto refs = references(source, {{1, 2, 3, 6, 7, 8, 9}});
    Scene scene(selectableTrails(source));
    const auto layer = sourceLayerOf(source);

    // Every trail at once: what all the tiles' buckets and the drape targets they cover cost.
    const auto all = scene.frameAfter([&] {
        for (int id = 1; id <= 9; ++id) {
            scene.renderer().setFeatureState(source, layer, std::to_string(id), selectedState());
        }
    });
    scene.settle();
    // Then only the centre tile's trail, back off.
    const auto one = scene.frameAfter(
        [&] { scene.renderer().setFeatureState(source, layer, "5", selectedState(false)); });
    scene.settle();
    // And one neighbour's.
    const auto other = scene.frameAfter(
        [&] { scene.renderer().setFeatureState(source, layer, "4", selectedState(false)); });
    scene.settle();
    RecordProperty("oneTileUploadBytes", std::to_string(one.uploadBytes));
    RecordProperty("oneTileBakes", std::to_string(one.bakes));
    RecordProperty("everyTileUploadBytes", std::to_string(all.uploadBytes));
    RecordProperty("everyTileBakes", std::to_string(all.bakes));

    EXPECT_GT(one.uploadBytes, 0u);
    EXPECT_EQ(one.uploadBytes, other.uploadBytes) << "two alike tiles uploaded different amounts";
    EXPECT_GE(all.uploadBytes, 4 * one.uploadBytes)
        << "one tile's change uploaded " << one.uploadBytes << " bytes, every tile's " << all.uploadBytes;
    EXPECT_GE(one.bakes, 1) << "the change did not re-bake the drape under its tile";
    EXPECT_GE(all.bakes, 4 * one.bakes) << "one tile's change baked " << one.bakes << " targets, every tile's "
                                        << all.bakes;

    const auto rest = scene.restCost();
    EXPECT_EQ(rest.uploadBytes, 0u);
    EXPECT_EQ(rest.bakes, 0);
    std::vector<int> selected;
    for (int id = 1; id <= 9; ++id) {
        if (id != 5 && id != 4) {
            selected.push_back(id);
        }
    }
    expectShowsSelected(scene, refs, selected);
}

// Removing a state that does not exist, and setting a state to the value it has, change nothing.
TEST_P(TerrainFeatureState, ANoOpChangeUploadsAndBakesNothing) {
    const std::string source = GetParam();
    const auto refs = references(source, {{centreFeature}});
    Scene scene(selectableTrails(source));
    const auto layer = sourceLayerOf(source);
    scene.frameAfter([&] { scene.renderer().setFeatureState(source, layer, "5", selectedState()); });
    scene.settle();

    const auto missingFeature = scene.frameAfter(
        [&] { scene.renderer().removeFeatureState(source, layer, std::string("999"), std::nullopt); });
    const auto missingKey = scene.frameAfter(
        [&] { scene.renderer().removeFeatureState(source, layer, std::string("5"), std::string("hover")); });
    const auto sameValue = scene.frameAfter(
        [&] { scene.renderer().setFeatureState(source, layer, "5", selectedState()); });
    EXPECT_EQ(missingFeature.uploadBytes, 0u) << "removing a missing feature's state uploaded";
    EXPECT_EQ(missingFeature.bakes, 0) << "removing a missing feature's state baked";
    EXPECT_EQ(missingKey.uploadBytes, 0u) << "removing a missing key uploaded";
    EXPECT_EQ(missingKey.bakes, 0) << "removing a missing key baked";
    EXPECT_EQ(sameValue.uploadBytes, 0u) << "setting the same value uploaded";
    EXPECT_EQ(sameValue.bakes, 0) << "setting the same value baked";
    expectShowsSelected(scene, refs, {centreFeature});
}

// A state set before the tile that holds the feature has loaded is drawn once it loads.
TEST_P(TerrainFeatureState, AStateSetBeforeTheTileLoadsIsDrawn) {
    const std::string source = GetParam();
    const auto refs = references(source, {{centreFeature}});
    Scene scene(selectableTrails(source), {}, [&](Scene& s) {
        s.renderer().setFeatureState(source, sourceLayerOf(source), "5", selectedState());
    });
    expectShowsSelected(scene, refs, {centreFeature});
    EXPECT_EQ(scene.restCost().uploadBytes, 0u);
}

// removeFeatureState with a feature and no key, and with no feature (the whole source layer).
TEST_P(TerrainFeatureState, RemovingStatesIsDrawn) {
    const std::string source = GetParam();
    const auto refs = references(source, {{4, 5, 6}, {4, 6}, {6}});
    Scene scene(selectableTrails(source));
    const auto layer = sourceLayerOf(source);
    scene.frameAfter([&] {
        for (const char* id : {"4", "5", "6"}) {
            scene.renderer().setFeatureState(source, layer, id, selectedState());
        }
    });
    expectShowsSelected(scene, refs, {4, 5, 6});

    const auto one = scene.frameAfter(
        [&] { scene.renderer().removeFeatureState(source, layer, std::string("5"), std::nullopt); });
    EXPECT_GT(one.uploadBytes, 0u);
    expectShowsSelected(scene, refs, {4, 6});
    EXPECT_EQ(scene.restCost().uploadBytes, 0u);

    scene.frameAfter([&] { scene.renderer().removeFeatureState(source, layer, std::nullopt, std::nullopt); });
    expectShowsSelected(scene, refs, {});
    EXPECT_EQ(scene.restCost().uploadBytes, 0u);

    // A state set again after the whole layer was cleared.
    scene.frameAfter([&] { scene.renderer().setFeatureState(source, layer, "6", selectedState()); });
    expectShowsSelected(scene, refs, {6});
}

INSTANTIATE_TEST_SUITE_P(Sources,
                         TerrainFeatureState,
                         ::testing::Values(std::string("overlay"), std::string("vt")),
                         [](const auto& info) { return info.param == "vt" ? "VectorTiles" : "GeoJSON"; });

// --- Feature state on the other bucket kinds: fill, fill-extrusion, symbol ---

namespace {

const std::string stateSelected = R"(["boolean", ["feature-state", "selected"], false])";

std::string idsSelected(const Selection& ids) {
    std::string list;
    for (const int id : ids) {
        list += (list.empty() ? "" : ", ") + std::to_string(id);
    }
    return ids.empty() ? "false" : R"(["in", ["id"], ["literal", [)" + list + "]]]";
}

// One layer of `kind` on the shapes, whose colour is chosen by `selected` (a boolean expression).
std::string shapesLayer(const std::string& kind, const std::string& selected) {
    const std::string color = R"(["case", )" + selected + R"(, "#2040e0", "#e02020"])";
    if (kind == "fill") {
        return R"({"id": "shapes", "type": "fill", "source": "shapes", "filter": ["==", ["geometry-type"], "Polygon"],
          "paint": {"fill-color": )" +
               color + "}}";
    }
    if (kind == "fill-extrusion") {
        return R"({"id": "shapes", "type": "fill-extrusion", "source": "shapes",
          "filter": ["==", ["geometry-type"], "Polygon"],
          "paint": {"fill-extrusion-color": )" +
               color + R"(, "fill-extrusion-height": 100}})";
    }
    // An SDF icon, so icon-color shows; drawn whatever the collisions.
    return R"({"id": "shapes", "type": "symbol", "source": "shapes", "filter": ["==", ["geometry-type"], "Point"],
      "layout": {"icon-image": "dot", "icon-size": 4, "icon-allow-overlap": true, "icon-ignore-placement": true},
      "paint": {"icon-color": )" +
           color + "}}";
}

void addDot(style::Style& style) {
    PremultipliedImage image({16, 16});
    for (uint32_t row = 0; row < 16; ++row) {
        for (uint32_t col = 0; col < 16; ++col) {
            uint8_t* px = image.data.get() + (static_cast<size_t>(row) * 16 + col) * 4;
            const bool inside = row >= 2 && row < 14 && col >= 2 && col < 14;
            px[0] = px[1] = px[2] = 0;
            px[3] = inside ? 255 : 0;
        }
    }
    style.addImage(std::make_unique<style::Image>("dot", std::move(image), 1.0f, /*sdf=*/true));
}

class TerrainFeatureStateBucket : public ::testing::TestWithParam<std::string> {};

} // namespace

// Each bucket kind uploads nothing at rest once a state is set, uploads only the tile whose
// feature changed, and draws the change. Fills are draped, so their bakes are checked too.
TEST_P(TerrainFeatureStateBucket, UploadsOnlyWhatChangedAndDrawsIt) {
    const std::string kind = GetParam();
    const Selection selected = {1, 2, 3, 6, 7, 8, 9};
    const auto refs = references(
        [&](const Selection& ids) { return shapesLayer(kind, idsSelected(ids)); }, {selected}, addDot);
    Scene scene(shapesLayer(kind, stateSelected), addDot);

    const auto all = scene.frameAfter([&] {
        for (int id = 1; id <= 9; ++id) {
            scene.renderer().setFeatureState("shapes", std::nullopt, std::to_string(id), selectedState());
        }
    });
    scene.settle();
    const auto one = scene.frameAfter(
        [&] { scene.renderer().setFeatureState("shapes", std::nullopt, "5", selectedState(false)); });
    scene.settle();
    const auto other = scene.frameAfter(
        [&] { scene.renderer().setFeatureState("shapes", std::nullopt, "4", selectedState(false)); });
    scene.settle();
    RecordProperty("oneTileUploadBytes", std::to_string(one.uploadBytes));
    RecordProperty("everyTileUploadBytes", std::to_string(all.uploadBytes));
    RecordProperty("oneTileBakes", std::to_string(one.bakes));
    RecordProperty("everyTileBakes", std::to_string(all.bakes));

    EXPECT_GT(one.uploadBytes, 0u) << "the change uploaded nothing";
    EXPECT_EQ(one.uploadBytes, other.uploadBytes) << "two alike tiles uploaded different amounts";
    EXPECT_GE(all.uploadBytes, 4 * one.uploadBytes)
        << "one tile's change uploaded " << one.uploadBytes << " bytes, every tile's " << all.uploadBytes;
    if (kind == "fill") {
        EXPECT_GE(one.bakes, 1) << "the change did not re-bake the drape under its tile";
        EXPECT_GE(all.bakes, 4 * one.bakes)
            << "one tile's change baked " << one.bakes << " targets, every tile's " << all.bakes;
    }
    const auto rest = scene.restCost();
    RecordProperty("restUploadBytes", std::to_string(rest.uploadBytes));
#if !MLN_RENDER_BACKEND_METAL
    if (kind == "symbol") {
        // Symbol placement re-runs on a timer and rewrites every icon's opacity vertices (16 bytes
        // a tile here), with or without feature state: on llvmpipe and lavapipe, whose frames are
        // slow, the rest window spans one or two placements (144-288 bytes measured, the same
        // with no state ever set). That is upstream placement, not feature state. A feature-state
        // re-upload at rest would rewrite every tile's paint vertices on every frame: at least
        // `restFrameCount` times the cost of changing every tile. Metal's frames are fast enough
        // that no placement falls in the window, and it checks for zero.
        EXPECT_LT(rest.uploadBytes, restFrameCount * all.uploadBytes / 2) << "vertex bytes uploaded at rest";
    } else
#endif
    {
        EXPECT_EQ(rest.uploadBytes, 0u) << "vertex bytes uploaded at rest";
    }
    EXPECT_EQ(rest.bakes, 0) << "drape targets baked at rest";
    expectShowsSelected(scene, refs, selected);
}

INSTANTIATE_TEST_SUITE_P(Kinds,
                         TerrainFeatureStateBucket,
                         ::testing::Values(std::string("fill"), std::string("fill-extrusion"), std::string("symbol")),
                         [](const auto& info) {
                             return info.param == "fill" ? "Fill"
                                                         : (info.param == "symbol" ? "Symbol" : "FillExtrusion");
                         });

// A reloaded tile (new data for the GeoJSON source, the same features) draws the states again.
TEST(TerrainFeatureStateReload, AReloadedTileDrawsTheState) {
    const auto refs = references("overlay", {{centreFeature}});
    Scene scene(selectableTrails("overlay"));
    scene.frameAfter([&] { scene.renderer().setFeatureState("overlay", std::nullopt, "5", selectedState()); });
    expectShowsSelected(scene, refs, {centreFeature});

    auto* geojson = static_cast<style::GeoJSONSource*>(scene.map.getStyle().getSource("overlay"));
    ASSERT_NE(geojson, nullptr);
    style::conversion::Error error;
    const auto data = style::conversion::convertJSON<GeoJSON>(trailsGeoJSON(), error);
    ASSERT_TRUE(data) << error.message;
    geojson->setGeoJSON(*data);
    expectShowsSelected(scene, refs, {centreFeature});
    EXPECT_EQ(scene.restCost().uploadBytes, 0u);
}

// --- The source's feature-state revision ---

namespace {

// Records what SourceFeatureState::coalesceChanges sends it.
class RecordingTile final : public Tile {
public:
    RecordingTile()
        : Tile(Kind::Geometry, OverscaledTileID(0, 0, 0), "source") {}
    std::unique_ptr<TileRenderData> createRenderData() override { return nullptr; }
    void cancel() override {}
    bool layerPropertiesUpdated(const Immutable<style::LayerProperties>&) override { return false; }
    void setFeatureState(const LayerFeatureStates& states_, uint64_t revision_) override {
        states = states_;
        revision = revision_;
    }
    LayerFeatureStates states;
    std::optional<uint64_t> revision;
};

} // namespace

// The revision moves when, and only when, a state changes, so tiles re-apply the states only then.
TEST(TerrainFeatureStateRevision, MovesOnlyOnARealChange) {
    SourceFeatureState source;
    RecordingTile tile;
    std::vector<RenderTile> tiles;
    tiles.emplace_back(UnwrappedTileID(0, 0, 0), tile);
    const auto coalesce = [&] {
        tile.revision.reset();
        source.coalesceChanges(tiles);
        return tile.revision;
    };
    const std::optional<std::string> layer("trails");

    EXPECT_EQ(coalesce(), std::nullopt) << "no state: nothing to send";
    source.updateState(layer, "5", selectedState());
    const auto first = coalesce();
    ASSERT_TRUE(first);
    EXPECT_EQ(coalesce(), first) << "a frame at rest";
    source.updateState(layer, "5", selectedState());
    EXPECT_EQ(coalesce(), first) << "the same value again";
    source.removeState(layer, std::string("999"), std::nullopt);
    EXPECT_EQ(coalesce(), first) << "removing a missing feature's state";
    source.removeState(layer, std::string("5"), std::string("hover"));
    EXPECT_EQ(coalesce(), first) << "removing a missing key";
    source.removeState(std::string("other"), std::nullopt, std::nullopt);
    EXPECT_EQ(coalesce(), first) << "removing a missing source layer";
    EXPECT_FALSE(tile.states.at("trails").contains("999")) << "a missing feature gained an entry";

    source.removeState(layer, std::string("5"), std::string("selected"));
    const auto removed = coalesce();
    EXPECT_GT(removed, first) << "removing a key";
    // The removed feature stays, with no state, for tiles that apply the states later.
    EXPECT_TRUE(tile.states.at("trails").at("5").empty());
    source.removeState(layer, std::nullopt, std::nullopt);
    EXPECT_EQ(coalesce(), removed) << "clearing a source layer whose states are all empty";

    source.updateState(layer, "6", selectedState(false));
    const auto set = coalesce();
    EXPECT_GT(set, removed) << "a new state";
    source.updateState(layer, "6", selectedState(true));
    const auto changed = coalesce();
    EXPECT_GT(changed, set) << "a new value";
    source.removeState(layer, std::string("6"), std::nullopt);
    const auto cleared = coalesce();
    EXPECT_GT(cleared, changed) << "removing a feature's states";
    source.updateState(layer, "6", selectedState(true));
    const auto again = coalesce();
    source.removeState(layer, std::nullopt, std::nullopt);
    EXPECT_GT(coalesce(), again) << "clearing a source layer";
    EXPECT_TRUE(tile.states.at("trails").at("6").empty());
}

// --- In-place image updates (R1(c), R3) ---

namespace {

// A 16×16 image whose stripes are the given colour. Every version has the same size, so an update
// is patched into the atlases in place. The outermost pixels never change (see
// terrain_paint_drape.test.cpp).
std::unique_ptr<style::Image> stripedImage(const std::string& id, uint8_t r, uint8_t g, uint8_t b) {
    PremultipliedImage image({16, 16});
    for (uint32_t row = 0; row < 16; ++row) {
        for (uint32_t col = 0; col < 16; ++col) {
            uint8_t* px = image.data.get() + (static_cast<size_t>(row) * 16 + col) * 4;
            const bool edge = row == 0 || row == 15 || col == 0 || col == 15;
            const bool stripe = !edge && (row / 4) % 2 == 0;
            px[0] = stripe ? r : 240;
            px[1] = stripe ? g : 240;
            px[2] = stripe ? b : 240;
            px[3] = 255;
        }
    }
    return std::make_unique<style::Image>(id, std::move(image), 1.0f);
}

// The trails in every tile, and an area in the centre tile drawn with `pattern`.
std::string patternLayers(const std::string& pattern) {
    return R"({"id": "trail", "type": "line", "source": "overlay",
      "filter": ["==", ["geometry-type"], "LineString"],
      "paint": {"line-color": "#e02020", "line-width": 8}},
    {"id": "area", "type": "fill", "source": "overlay", "filter": ["==", ["geometry-type"], "Polygon"],
      "paint": {"fill-pattern": )" +
           pattern + "}}";
}

void addImages(style::Style& style) {
    style.addImage(stripedImage("stripe", 200, 40, 30));
    style.addImage(stripedImage("dots", 30, 160, 40));
    style.addImage(stripedImage("marker", 20, 20, 20));
}

struct PatternScene : Scene {
    explicit PatternScene(const std::string& pattern)
        : Scene(patternLayers(pattern), addImages) {}
    struct Layers {
        std::string json;
    };
    explicit PatternScene(const Layers& layers)
        : Scene(layers.json, addImages) {}

    // The drape targets the trails cover: every target in view.
    int allTargets() {
        const auto cost = frameAfter(
            [&] { static_cast<style::LineLayer*>(map.getStyle().getLayer("trail"))->setLineColor(Color::green()); });
        settle();
        return cost.bakes;
    }

    // Updates `id` in place and returns what that frame baked.
    int bakesForUpdateOf(const std::string& id, uint8_t shade) {
        return frameAfter([&] { map.getStyle().addImage(stripedImage(id, shade, shade, 255 - shade)); }).bakes;
    }
};

} // namespace

// R1(c): an image no draped layer draws, updated on every frame, bakes nothing; an update of the
// image a draped pattern draws bakes that layer's targets, once per update.
TEST(TerrainImageUpdate, OnlyTheUpdatedPatternsTargetsBake) {
    PatternScene scene(R"("stripe")");
    const int all = scene.allTargets();
    ASSERT_GT(all, 1);

    int unusedBakes = 0;
    for (int frame = 0; frame < restFrameCount; ++frame) {
        unusedBakes += scene.bakesForUpdateOf("marker", static_cast<uint8_t>(10 * frame));
    }
    RecordProperty("unusedImageBakes", std::to_string(unusedBakes));
    EXPECT_EQ(unusedBakes, 0) << "an image no layer draws, updated on each of " << restFrameCount
                              << " frames, baked the drape";

    std::vector<int> patternBakes;
    for (int update = 0; update < 3; ++update) {
        patternBakes.push_back(scene.bakesForUpdateOf("stripe", static_cast<uint8_t>(60 * update)));
        EXPECT_EQ(scene.restCost(2).bakes, 0) << "the drape re-bakes at rest after a pattern update";
    }
    RecordProperty("patternImageBakes", std::to_string(patternBakes[0]));
    RecordProperty("allTargets", std::to_string(all));
    EXPECT_GE(patternBakes[0], 1) << "updating the pattern's image did not re-bake its drape";
    // The area lies in the centre tile, under a quarter of the targets.
    EXPECT_LE(patternBakes[0] * 4, all) << "updating the pattern's image re-baked targets the pattern does not cover";
    EXPECT_EQ(patternBakes[1], patternBakes[0]);
    EXPECT_EQ(patternBakes[2], patternBakes[0]);
}

// A line-pattern (the centre tile's trail only, over the plain trails): an unused image bakes
// nothing, and the pattern's own image bakes only that trail's targets.
TEST(TerrainImageUpdate, OnlyTheUpdatedLinePatternsTargetsBake) {
    PatternScene scene(PatternScene::Layers{R"({"id": "trail", "type": "line", "source": "overlay",
      "filter": ["==", ["geometry-type"], "LineString"], "paint": {"line-color": "#e02020", "line-width": 8}},
    {"id": "trail-pattern", "type": "line", "source": "overlay", "filter": ["==", ["id"], 5],
      "paint": {"line-pattern": "stripe", "line-width": 16}})"});
    const int all = scene.allTargets();
    ASSERT_GT(all, 1);
    int unusedBakes = 0;
    for (int frame = 0; frame < restFrameCount; ++frame) {
        unusedBakes += scene.bakesForUpdateOf("marker", static_cast<uint8_t>(10 * frame));
    }
    EXPECT_EQ(unusedBakes, 0) << "an image no layer draws baked the drape";
    const int patternBakes = scene.bakesForUpdateOf("stripe", 60);
    RecordProperty("patternImageBakes", std::to_string(patternBakes));
    RecordProperty("allTargets", std::to_string(all));
    EXPECT_GE(patternBakes, 1) << "updating the line pattern's image did not re-bake its drape";
    EXPECT_LE(patternBakes * 4, all) << "updating the line pattern's image re-baked targets it does not cover";
    EXPECT_EQ(scene.restCost(2).bakes, 0);
}

// A background-pattern covers every target: an unused image bakes nothing, and the pattern's own
// image bakes the drape.
TEST(TerrainImageUpdate, OnlyAnUpdateOfTheBackgroundPatternsImageBakes) {
    PatternScene scene(PatternScene::Layers{R"({"id": "trail", "type": "line", "source": "overlay",
      "filter": ["==", ["geometry-type"], "LineString"], "paint": {"line-color": "#e02020", "line-width": 8}},
    {"id": "background-pattern", "type": "background", "paint": {"background-pattern": "stripe"}})"});
    int unusedBakes = 0;
    for (int frame = 0; frame < restFrameCount; ++frame) {
        unusedBakes += scene.bakesForUpdateOf("marker", static_cast<uint8_t>(10 * frame));
    }
    EXPECT_EQ(unusedBakes, 0) << "an image no layer draws baked the drape";
    const int patternBakes = scene.bakesForUpdateOf("stripe", 60);
    RecordProperty("patternImageBakes", std::to_string(patternBakes));
    EXPECT_GE(patternBakes, 1) << "updating the background pattern's image did not re-bake the drape";
    EXPECT_EQ(scene.restCost(2).bakes, 0);
}

// A data-driven pattern can draw only the images its expression names.
TEST(TerrainImageUpdate, ADataDrivenPatternBakesOnlyForItsImages) {
    PatternScene scene(R"(["match", ["get", "kind"], "a", "stripe", "dots"])");
    EXPECT_EQ(scene.bakesForUpdateOf("marker", 10), 0);
    EXPECT_GT(scene.bakesForUpdateOf("dots", 20), 0) << "an image the match can draw";
    EXPECT_GT(scene.bakesForUpdateOf("stripe", 30), 0) << "the image the match draws";
}

// A zoom-dependent pattern can draw any of its stops' images (crossfaded across zooms).
TEST(TerrainImageUpdate, AZoomDependentPatternBakesOnlyForItsImages) {
    PatternScene scene(R"(["step", ["zoom"], ["image", "dots"], 10, ["image", "stripe"]])");
    EXPECT_EQ(scene.bakesForUpdateOf("marker", 10), 0);
    EXPECT_GT(scene.bakesForUpdateOf("stripe", 30), 0);
}

// A pattern read from the data may draw any image: every in-place update bakes it.
TEST(TerrainImageUpdate, APatternFromTheDataBakesForAnyImage) {
    PatternScene scene(R"(["get", "pattern"])");
    EXPECT_GT(scene.bakesForUpdateOf("marker", 10), 0);
    EXPECT_GT(scene.bakesForUpdateOf("stripe", 30), 0);
}

// The images each pattern value can draw.
TEST(TerrainImageUpdate, PatternImagesOfAValue) {
    using style::PropertyValue;
    using style::expression::Image;
    const auto parse = [](const std::string& json) {
        style::conversion::Error error;
        auto value = style::conversion::convertJSON<PropertyValue<Image>>(json, error, true, false);
        EXPECT_TRUE(value) << json << ": " << error.message;
        return RenderLayer::patternImagesOf(value ? *value : PropertyValue<Image>());
    };
    const auto ids = [](const RenderLayer::PatternImages& images) {
        return std::set<std::string>(images.ids.begin(), images.ids.end());
    };

    EXPECT_TRUE(RenderLayer::patternImagesOf(PropertyValue<Image>()).ids.empty());
    EXPECT_FALSE(RenderLayer::patternImagesOf(PropertyValue<Image>()).mayDraw("stripe"));

    const auto constant = parse(R"("stripe")");
    EXPECT_EQ(ids(constant), std::set<std::string>({"stripe"}));
    EXPECT_FALSE(constant.anyImage);
    EXPECT_TRUE(constant.mayDraw("stripe"));
    EXPECT_FALSE(constant.mayDraw("marker"));

    const auto step = parse(R"(["step", ["zoom"], "a", 10, "b", 14, ["image", "c"]])");
    EXPECT_EQ(ids(step), std::set<std::string>({"a", "b", "c"}));
    EXPECT_FALSE(step.anyImage);

    const auto match = parse(R"(["match", ["get", "kind"], "x", "a", ["y", "z"], "b", "c"])");
    EXPECT_EQ(ids(match), std::set<std::string>({"a", "b", "c"}));
    EXPECT_FALSE(match.anyImage);

    const auto cased = parse(R"(["case", ["boolean", ["feature-state", "selected"], false], "a", "b"])");
    EXPECT_EQ(ids(cased), std::set<std::string>({"a", "b"}));
    EXPECT_FALSE(cased.anyImage);

    EXPECT_TRUE(parse(R"(["get", "pattern"])").anyImage);
    EXPECT_TRUE(parse(R"(["get", "pattern"])").mayDraw("anything"));
    EXPECT_TRUE(parse(R"(["concat", "trail-", ["get", "kind"]])").anyImage);
    EXPECT_TRUE(parse(R"(["coalesce", ["get", "pattern"], "a"])").anyImage);
    EXPECT_TRUE(parse(R"(["image", ["get", "pattern"]])").anyImage);

    // An expression with no possible outputs (an error) is unknown too.
    const auto error = RenderLayer::patternImagesOf(
        style::PropertyExpression<Image>(std::make_unique<style::expression::Error>("no outputs")));
    EXPECT_TRUE(error.anyImage);
    EXPECT_TRUE(error.mayDraw("anything"));
}
