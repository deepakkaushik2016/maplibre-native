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
#include <mln/renderer/renderer.hpp>
#include <mln/storage/response.hpp>
#include <mln/style/layers/background_layer.hpp>
#include <mln/style/layers/color_relief_layer.hpp>
#include <mln/style/layers/fill_layer.hpp>
#include <mln/style/layers/heatmap_layer.hpp>
#include <mln/style/layers/hillshade_layer.hpp>
#include <mln/style/layers/line_layer.hpp>
#include <mln/style/layers/raster_layer.hpp>
#include <mln/style/style.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>

using namespace mln;

// A paint change on a draped layer must show on the next frame, and the drape must not re-render
// while nothing changes. The terrain caches each drape target's texture and re-renders it only when
// its content signature moves; a paint change (or a feature-state change) keeps the same tiles and
// buckets, so it must move the signature itself. Each test draws a scene at rest, changes one
// paint property, draws ONE frame and compares it with a scene loaded with the new paint from the
// start. The app changes these properties at rest all the time: the selected trail's colour and
// width, the hillshade and colour-relief toggles, the user-location accuracy fill.

namespace {

constexpr uint32_t viewWidth = 390;
constexpr uint32_t viewHeight = 844;
constexpr int demZoom = 10;
constexpr uint32_t demSize = 256;
constexpr double kmPerDegree = 6371.0088 * M_PI / 180.0;

// The ridge of terrain_motion_drape.test.cpp: an east-west crest along alpsLat over a valley.
constexpr double alpsLat = 47.265;
constexpr double alpsLng = 11.404;

double ridgeMeters(double lat, double lng) {
    const double northKm = (lat - alpsLat) * kmPerDegree;
    const double eastKm = (lng - alpsLng) * kmPerDegree * std::cos(util::deg2rad(alpsLat));
    const double ridge = std::exp(-std::pow(northKm / 4.0, 2.0));
    const double crest = 0.9 + 0.1 * std::cos(2.0 * M_PI * eastKm / 12.0);
    return 575.0 + (2300.0 - 575.0) * ridge * crest;
}

LatLng waypoint(double eastKm, double northKm) {
    return {alpsLat + northKm / kmPerDegree, alpsLng + eastKm / (kmPerDegree * std::cos(util::deg2rad(alpsLat)))};
}

// The view looks north from the ridge's northern flank, so it sees slopes (for the hillshade and
// the colour relief) and the valley where the trail and the area lie.
constexpr double viewNorthKm = 5.0;

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

// A raster tile: a checkerboard of two colours, so a raster-opacity change shows everywhere.
std::string rasterTile() {
    PremultipliedImage image({256, 256});
    for (uint32_t row = 0; row < 256; ++row) {
        for (uint32_t col = 0; col < 256; ++col) {
            uint8_t* px = image.data.get() + (static_cast<size_t>(row) * 256 + col) * 4;
            const bool dark = ((row / 32) + (col / 32)) % 2 == 0;
            px[0] = dark ? 40 : 230;
            px[1] = dark ? 60 : 120;
            px[2] = dark ? 160 : 40;
            px[3] = 255;
        }
    }
    return encodePNG(image);
}

std::string lonLat(const LatLng& p) {
    return "[" + std::to_string(p.longitude()) + ", " + std::to_string(p.latitude()) + "]";
}

// A trail (feature 1) across the view and an area (feature 2) below it, as GeoJSON.
std::string overlayData() {
    const LatLng a = waypoint(-3.0, viewNorthKm + 0.6), b = waypoint(0.0, viewNorthKm + 0.9),
                 c = waypoint(3.0, viewNorthKm + 0.6);
    const LatLng sw = waypoint(-1.2, viewNorthKm - 0.9), se = waypoint(1.2, viewNorthKm - 0.9),
                 ne = waypoint(1.2, viewNorthKm + 0.2), nw = waypoint(-1.2, viewNorthKm + 0.2);
    // Points for the heatmap, on a grid around the view's centre.
    std::string points;
    for (int i = -3; i <= 3; ++i) {
        for (int j = -3; j <= 3; ++j) {
            points += R"JSON(,
      {"type": "Feature", "properties": {}, "geometry": {"type": "Point", "coordinates": )JSON" +
                      lonLat(waypoint(0.25 * i, viewNorthKm + 0.25 * j)) + "}}";
        }
    }
    return R"JSON({"type": "FeatureCollection", "features": [
      {"type": "Feature", "id": 1, "properties": {},
       "geometry": {"type": "LineString", "coordinates": [)JSON" +
           lonLat(a) + ", " + lonLat(b) + ", " + lonLat(c) + R"JSON(]}},
      {"type": "Feature", "id": 2, "properties": {},
       "geometry": {"type": "Polygon", "coordinates": [[)JSON" +
           lonLat(sw) + ", " + lonLat(se) + ", " + lonLat(ne) + ", " + lonLat(nw) + ", " + lonLat(sw) +
           R"JSON(]]}})JSON" + points + "]}";
}

// Paint transitions are off (duration 0) unless a test sets one, so a change is whole on the next
// frame, as the app's highlight is.
std::string paintStyle(const std::string& layers) {
    // The image source is in the style only when a layer uses it: an unused one never loads.
    std::string imageSource;
    if (layers.find(R"("source": "image")") != std::string::npos) {
        imageSource = R"STYLE("image": {
      "type": "image",
      "url": "http://example.com/image.png",
      "coordinates": [)STYLE" +
                      lonLat(waypoint(-1.5, viewNorthKm + 1.5)) + ", " + lonLat(waypoint(1.5, viewNorthKm + 1.5)) +
                      ", " + lonLat(waypoint(1.5, viewNorthKm - 1.0)) + ", " +
                      lonLat(waypoint(-1.5, viewNorthKm - 1.0)) + R"STYLE(]
    },)STYLE";
    }
    return R"STYLE({
  "version": 8,
  "transition": {"duration": 0, "delay": 0},
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"],
      "encoding": "terrarium",
      "minzoom": )STYLE" +
           std::to_string(demZoom) + R"STYLE(,
      "maxzoom": )STYLE" +
           std::to_string(demZoom) + R"STYLE(,
      "tileSize": 256
    },
    "raster": {
      "type": "raster",
      "tiles": ["http://example.com/raster/{z}-{x}-{y}.png"],
      "tileSize": 256
    },
    )STYLE" +
           imageSource + R"STYLE(
    "overlay": {"type": "geojson", "data": )STYLE" +
           overlayData() + R"STYLE(}
  },
  "terrain": {"source": "dem", "exaggeration": 1.25},
  "layers": [
)STYLE" + layers +
           R"STYLE(
  ]
})STYLE";
}

const std::string backgroundLayer =
    R"JSON({"id": "background", "type": "background", "paint": {"background-color": "#dddddd"}})JSON";
const std::string hillshadeLayer =
    R"JSON({"id": "hillshade", "type": "hillshade", "source": "dem",
    "paint": {"hillshade-shadow-color": "#000000", "hillshade-highlight-color": "#ffffff"}})JSON";
const std::string colorReliefLayer = R"JSON({"id": "relief", "type": "color-relief", "source": "dem",
    "paint": {"color-relief-color": ["interpolate", ["linear"], ["elevation"],
        0, "rgb(20, 120, 40)", 2500, "rgb(150, 90, 30)"]}})JSON";
const std::string imageLayer = R"JSON({"id": "image", "type": "raster", "source": "image"})JSON";
const std::string heatmapLayer = R"JSON({"id": "heat", "type": "heatmap", "source": "overlay",
    "filter": ["==", ["geometry-type"], "Point"],
    "paint": {"heatmap-radius": 40, "heatmap-intensity": 1, "heatmap-opacity": 1}})JSON";
const std::string rasterLayer = R"JSON({"id": "raster", "type": "raster", "source": "raster"})JSON";
const std::string fillLayer = R"JSON({"id": "area", "type": "fill", "source": "overlay",
    "filter": ["==", ["geometry-type"], "Polygon"],
    "paint": {"fill-color": "#20c040", "fill-opacity": 0.8}})JSON";
const std::string lineLayer = R"JSON({"id": "trail", "type": "line", "source": "overlay",
    "filter": ["==", ["geometry-type"], "LineString"],
    "paint": {"line-color": "#e02020", "line-width": 28}})JSON";
// The selected-trail highlight driven by feature state: blue and wider when selected.
const std::string featureStateLineLayer = R"JSON({"id": "trail", "type": "line", "source": "overlay",
    "filter": ["==", ["geometry-type"], "LineString"],
    "paint": {
      "line-color": ["case", ["boolean", ["feature-state", "selected"], false], "#2040e0", "#e02020"],
      "line-width": ["case", ["boolean", ["feature-state", "selected"], false], 28, 16]}})JSON";
// What the feature-state line draws once feature 1 is selected.
const std::string selectedLineLayer = R"JSON({"id": "trail", "type": "line", "source": "overlay",
    "filter": ["==", ["geometry-type"], "LineString"],
    "paint": {"line-color": "#2040e0", "line-width": 28}})JSON";

std::string join(std::initializer_list<std::string> layers) {
    std::string out;
    for (const auto& layer : layers) {
        out += (out.empty() ? "    " : ",\n    ") + layer;
    }
    return out;
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

// A phone-sized map at rest over the ridge, pitched, run continuously as in the app.
struct PaintScene {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    HeadlessFrontend frontend{{viewWidth, viewHeight}, 1};
    FrameObserver observer;
    MapAdapter map{
        frontend, observer, fileSource, MapOptions().withMapMode(MapMode::Continuous).withSize(frontend.getSize())};

    explicit PaintScene(const std::string& layers) {
        fileSource->tileResponse = [](const Resource& resource) -> std::optional<Response> {
            Response res;
            if (!resource.tileData) {
                res.noContent = true;
                return res;
            }
            const CanonicalTileID id(
                resource.tileData->z, static_cast<uint32_t>(resource.tileData->x), resource.tileData->y);
            if (resource.url.find("/raster/") != std::string::npos) {
                res.data = std::make_shared<std::string>(rasterTile());
            } else if (resource.url.find("/dem/") != std::string::npos && id.z == demZoom) {
                res.data = std::make_shared<std::string>(ridgeTile(id));
            } else {
                res.error = std::make_unique<Response::Error>(Response::Error::Reason::NotFound, "no such tile");
            }
            return res;
        };
        fileSource->imageResponse = [](const Resource&) -> std::optional<Response> {
            Response res;
            res.data = std::make_shared<std::string>(rasterTile());
            return res;
        };
        map.getStyle().loadJSON(paintStyle(layers));
        map.jumpTo(
            CameraOptions().withCenter(waypoint(0, viewNorthKm)).withZoom(13.0).withPitch(45.0).withBearing(0.0));
        settle();
    }

    // Runs the map until every tile and image has arrived, been parsed and drawn, and a frame has
    // found nothing more to draw (at most 30 s).
    void settle() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        const int start = observer.frames;
        observer.idleFrame = false;
        const auto quiet = [&] {
            // The image source's image arrives with no tile event; wait for it too.
            return observer.frames > start && observer.parsing.empty() && observer.idleFrame && imageLoaded();
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

    bool imageLoaded() {
        const auto* image = map.getStyle().getSource("image");
        return image == nullptr || image->loaded;
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

    // Draws `count` frames with nothing changed (repaints at rest).
    void restFrames(int count) {
        for (int i = 0; i < count; ++i) {
            map.triggerRepaint();
            ASSERT_TRUE(nextFrame());
        }
    }

    int drapeTargetsRendered() { return frontend.getBackend()->getContext().renderingStats().numDrapeTargetsRendered; }

    PremultipliedImage image() {
        gfx::BackendScope guard{*frontend.getBackend()};
        return frontend.readStillImage();
    }

    template <typename L>
    L& layer(const std::string& id) {
        auto* found = map.getStyle().getLayer(id);
        EXPECT_NE(found, nullptr) << id;
        return *static_cast<L*>(found);
    }
};

// The share of pixels where the two frames differ by more than a few levels in a channel.
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

// A frame matches the reference when at most this share of its pixels differ (two scenes drawn
// alike match exactly; the margin takes the odd anti-aliased edge).
constexpr double maxMismatch = 0.002;
// A paint change the test can see moves at least this share of the frame.
constexpr double minChange = 0.01;
// Rest frames drawn before and after the change, which must re-render no drape target.
constexpr int restFrameCount = 8;

// Draws `before` at rest, applies `change`, draws one frame, and compares it with a scene loaded
// as `after`. Also checks that the drape re-renders on the change frame and never at rest.
void expectNextFrameShows(const std::string& before,
                          const std::function<void(PaintScene&)>& change,
                          const std::string& after,
                          bool draped = true) {
    const PremultipliedImage reference = PaintScene(after).image();

    PaintScene scene(before);
    const PremultipliedImage old = scene.image();
    ASSERT_GE(differingShare(old, reference), minChange)
        << "the change does not show even in a fresh scene: the test proves nothing";

    // At rest the drape is never re-rendered.
    const int beforeRest = scene.drapeTargetsRendered();
    scene.restFrames(restFrameCount);
    EXPECT_EQ(scene.drapeTargetsRendered(), beforeRest) << "the drape re-rendered while nothing changed";

    const int beforeChange = scene.drapeTargetsRendered();
    change(scene);
    ASSERT_TRUE(scene.nextFrame());
    const double mismatch = differingShare(scene.image(), reference);
    EXPECT_LE(mismatch, maxMismatch) << mismatch * 100.0
                                     << " % of the frame after the change still differs from the new paint";
    if (draped) {
        EXPECT_GT(scene.drapeTargetsRendered(), beforeChange) << "the change did not re-render the drape";
    }

    // Once the change has been drawn, the drape is cached again.
    scene.settle();
    const int afterSettle = scene.drapeTargetsRendered();
    scene.restFrames(restFrameCount);
    EXPECT_EQ(scene.drapeTargetsRendered(), afterSettle) << "the drape re-renders at rest after the change";
    EXPECT_LE(differingShare(scene.image(), reference), maxMismatch);
}

const std::string lineBlue = R"JSON({"id": "trail", "type": "line", "source": "overlay",
    "filter": ["==", ["geometry-type"], "LineString"],
    "paint": {"line-color": "#2040e0", "line-width": 28}})JSON";

} // namespace

// The selected trail's highlight: its colour and width change.
TEST(TerrainPaintDrape, LineColorAndWidthShowOnTheNextFrame) {
    expectNextFrameShows(
        join({backgroundLayer, hillshadeLayer, lineLayer}),
        [](PaintScene& scene) {
            auto& line = scene.layer<style::LineLayer>("trail");
            line.setLineColor(Color{0x20 / 255.0f, 0x40 / 255.0f, 0xe0 / 255.0f, 1.0f});
            line.setLineWidth(28.0f);
        },
        join({backgroundLayer, hillshadeLayer, lineBlue}));
}

// A fill's opacity (the user-location accuracy area, a park).
TEST(TerrainPaintDrape, FillOpacityShowsOnTheNextFrame) {
    const std::string faded = R"JSON({"id": "area", "type": "fill", "source": "overlay",
        "filter": ["==", ["geometry-type"], "Polygon"],
        "paint": {"fill-color": "#20c040", "fill-opacity": 0.3}})JSON";
    expectNextFrameShows(
        join({backgroundLayer, hillshadeLayer, fillLayer}),
        [](PaintScene& scene) { scene.layer<style::FillLayer>("area").setFillOpacity(0.3f); },
        join({backgroundLayer, hillshadeLayer, faded}));
}

TEST(TerrainPaintDrape, HillshadeColorShowsOnTheNextFrame) {
    const std::string red = R"JSON({"id": "hillshade", "type": "hillshade", "source": "dem",
        "paint": {"hillshade-shadow-color": "#ff0000", "hillshade-highlight-color": "#ffffff"}})JSON";
    expectNextFrameShows(
        join({backgroundLayer, hillshadeLayer}),
        [](PaintScene& scene) {
            scene.layer<style::HillshadeLayer>("hillshade")
                .setHillshadeShadowColor(std::vector<Color>{Color{1.0f, 0.0f, 0.0f, 1.0f}});
        },
        join({backgroundLayer, red}));
}

TEST(TerrainPaintDrape, ColorReliefOpacityShowsOnTheNextFrame) {
    const std::string faded = R"JSON({"id": "relief", "type": "color-relief", "source": "dem",
        "paint": {"color-relief-opacity": 0.4, "color-relief-color": ["interpolate", ["linear"], ["elevation"],
            0, "rgb(20, 120, 40)", 2500, "rgb(150, 90, 30)"]}})JSON";
    expectNextFrameShows(
        join({backgroundLayer, colorReliefLayer}),
        [](PaintScene& scene) { scene.layer<style::ColorReliefLayer>("relief").setColorReliefOpacity(0.4f); },
        join({backgroundLayer, faded}));
}

TEST(TerrainPaintDrape, RasterOpacityShowsOnTheNextFrame) {
    const std::string faded = R"JSON({"id": "raster", "type": "raster", "source": "raster",
        "paint": {"raster-opacity": 0.3}})JSON";
    expectNextFrameShows(
        join({backgroundLayer, rasterLayer}),
        [](PaintScene& scene) { scene.layer<style::RasterLayer>("raster").setRasterOpacity(0.3f); },
        join({backgroundLayer, faded}));
}

// An image source's raster is a plain layer group (not a tile group), drawn to the screen with
// terrain on, not into the drape.
TEST(TerrainPaintDrape, ImageRasterOpacityShowsOnTheNextFrame) {
#if MLN_RENDER_BACKEND_OPENGL
    // The OpenGL backend draws no image-source raster with terrain on, before or after a paint
    // change (a fresh scene shows only the background): a separate defect, not a stale drape.
    GTEST_SKIP() << "an image-source raster is not drawn with terrain on OpenGL";
#endif
    const std::string faded = R"JSON({"id": "image", "type": "raster", "source": "image",
        "paint": {"raster-opacity": 0.3}})JSON";
    expectNextFrameShows(
        join({backgroundLayer, imageLayer}),
        [](PaintScene& scene) { scene.layer<style::RasterLayer>("image").setRasterOpacity(0.3f); },
        join({backgroundLayer, faded}),
        /*draped=*/false);
}

// A heatmap is composited from its own target by a plain layer group, drawn to the screen.
TEST(TerrainPaintDrape, HeatmapOpacityShowsOnTheNextFrame) {
    const std::string faded = R"JSON({"id": "heat", "type": "heatmap", "source": "overlay",
        "filter": ["==", ["geometry-type"], "Point"],
        "paint": {"heatmap-radius": 40, "heatmap-intensity": 1, "heatmap-opacity": 0.3}})JSON";
    expectNextFrameShows(
        join({backgroundLayer, heatmapLayer}),
        [](PaintScene& scene) { scene.layer<style::HeatmapLayer>("heat").setHeatmapOpacity(0.3f); },
        join({backgroundLayer, faded}),
        /*draped=*/false);
}

TEST(TerrainPaintDrape, BackgroundColorShowsOnTheNextFrame) {
    const std::string blue =
        R"JSON({"id": "background", "type": "background", "paint": {"background-color": "#4060a0"}})JSON";
    expectNextFrameShows(
        join({backgroundLayer, lineLayer}),
        [](PaintScene& scene) {
            scene.layer<style::BackgroundLayer>("background")
                .setBackgroundColor(Color{0x40 / 255.0f, 0x60 / 255.0f, 0xa0 / 255.0f, 1.0f});
        },
        join({blue, lineLayer}));
}

// The highlight as the app may drive it: a data-driven line paint on feature state. The bucket's
// paint attributes are updated in place, so the tiles and buckets stay the same.
TEST(TerrainPaintDrape, FeatureStateOnALineShowsOnTheNextFrame) {
    expectNextFrameShows(
        join({backgroundLayer, hillshadeLayer, featureStateLineLayer}),
        [](PaintScene& scene) {
            FeatureState selected;
            selected["selected"] = true;
            scene.frontend.getRenderer()->setFeatureState("overlay", {}, "1", selected);
            scene.map.triggerRepaint();
        },
        join({backgroundLayer, hillshadeLayer, selectedLineLayer}));
}

// A paint transition re-renders the drape while it runs, ends at the new paint, and the drape is
// cached again once it has ended.
TEST(TerrainPaintDrape, LineColorTransitionEndsAtTheNewPaint) {
    const PremultipliedImage reference = PaintScene(join({backgroundLayer, hillshadeLayer, lineBlue})).image();
    PaintScene scene(join({backgroundLayer, hillshadeLayer, lineLayer}));
    auto& line = scene.layer<style::LineLayer>("trail");
    line.setLineColorTransition(style::TransitionOptions{Milliseconds(300), Milliseconds(0)});
    line.setLineWidthTransition(style::TransitionOptions{Milliseconds(300), Milliseconds(0)});
    line.setLineColor(Color{0x20 / 255.0f, 0x40 / 255.0f, 0xe0 / 255.0f, 1.0f});
    line.setLineWidth(28.0f);
    scene.settle();
    EXPECT_LE(differingShare(scene.image(), reference), maxMismatch) << "the transition did not end at the new paint";
    const int afterSettle = scene.drapeTargetsRendered();
    scene.restFrames(restFrameCount);
    EXPECT_EQ(scene.drapeTargetsRendered(), afterSettle) << "the drape re-renders at rest after the transition";
}
