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
#include <vector>

using namespace mln;

// The terrain must never go blank while the camera moves. These tests fly over a DEM that exists
// at one zoom only (minzoom = maxzoom), as the synthetic Alps fixture of the Flutter terrain tests
// and bounds-limited park DEMs do, so no DEM tile has a parent to stand in for it. The DEM layer
// is a colour relief, opaque everywhere the DEM is drawn, over a background of another colour, so
// a pixel of the background colour below the horizon is ground the frame left blank (the drape
// target is cleared to the background colour too, so a mesh drawn without its drape counts).

namespace {

constexpr uint32_t viewWidth = 390;
constexpr uint32_t viewHeight = 844;
constexpr int demZoom = 10;
constexpr uint32_t demSize = 256;
constexpr double kmPerDegree = 6371.0088 * M_PI / 180.0;
constexpr double exaggeration = 1.25;

// The Flutter fixture's ridge (integration_test/shared/alps_dem.dart): an east-west crest along
// alpsLat, 2300 m high at alpsLng and every 12 km east and west, falling off north and south to a
// 575 m valley floor.
constexpr double alpsLat = 47.265;
constexpr double alpsLng = 11.404;
constexpr double valleyFloor = 575.0;
constexpr double summit = 2300.0;

double ridgeMeters(double lat, double lng) {
    const double northKm = (lat - alpsLat) * kmPerDegree;
    const double eastKm = (lng - alpsLng) * kmPerDegree * std::cos(util::deg2rad(alpsLat));
    const double ridge = std::exp(-std::pow(northKm / 4.0, 2.0));
    const double crest = 0.9 + 0.1 * std::cos(2.0 * M_PI * eastKm / 12.0);
    return valleyFloor + (summit - valleyFloor) * ridge * crest;
}

LatLng waypoint(double eastKm, double northKm) {
    return {alpsLat + northKm / kmPerDegree, alpsLng + eastKm / (kmPerDegree * std::cos(util::deg2rad(alpsLat)))};
}

// The DEM tile z/x/y of the ridge in Terrarium, each pixel holding the height at its centre.
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

// The background's colour, and so the colour of a drape target before anything is drawn into it.
constexpr uint8_t backgroundRGB[3] = {0xdd, 0xdd, 0xdd};

std::string motionStyle(const std::string& demLayer) {
    return R"STYLE({
  "version": 8,
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
    }
  },
  "terrain": {"source": "dem", "exaggeration": )STYLE" +
           std::to_string(exaggeration) + R"STYLE(},
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "#dddddd"}},
    )STYLE" +
           demLayer +
           R"STYLE(
  ]
})STYLE";
}

// Opaque everywhere: green in the valleys to brown on the crest.
const std::string colorRelief = R"JSON({"id": "relief", "type": "color-relief", "source": "dem",
    "paint": {"color-relief-color": ["interpolate", ["linear"], ["elevation"],
        0, "rgb(20, 120, 40)", 2500, "rgb(150, 90, 30)"]}})JSON";

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
            transitionsEnded++;
        }
    }
    int frames = 0;
    std::set<std::pair<OverscaledTileID, std::string>> parsing;
    bool idleFrame = false;
    /// Camera transitions that have run to their end.
    int transitionsEnded = 0;
};

// A phone-sized map at the fixture's camera, run continuously so frames are drawn while the
// camera moves, as in the app. The file source answers the DEM tiles at zoom demZoom (on its
// 1 ms tick) and 404s every other zoom, as the fixture's server does.
struct MotionScene {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    HeadlessFrontend frontend{{viewWidth, viewHeight}, 1};
    FrameObserver observer;
    MapAdapter map{
        frontend, observer, fileSource, MapOptions().withMapMode(MapMode::Continuous).withSize(frontend.getSize())};
    std::map<CanonicalTileID, int> demRequests;

    /// `rebuildDrawables`: every layer drops its drawables and builds them anew each frame, as the
    /// renderer does on the Android emulator's GL (Renderer::enableAndroidEmulatorGoldfishMitigation,
    /// which the FFI turns on there; it has an effect on OpenGL builds only).
    explicit MotionScene(bool rebuildDrawables = false) {
        fileSource->tileResponse = [this](const Resource& resource) -> std::optional<Response> {
            Response res;
            if (!resource.tileData || resource.url.find("/dem/") == std::string::npos) {
                res.noContent = true;
                return res;
            }
            const CanonicalTileID id(
                resource.tileData->z, static_cast<uint32_t>(resource.tileData->x), resource.tileData->y);
            ++demRequests[id];
            if (id.z != demZoom) {
                res.error = std::make_unique<Response::Error>(Response::Error::Reason::NotFound, "no such zoom");
                return res;
            }
            res.data = std::make_shared<std::string>(ridgeTile(id));
            return res;
        };
#if MLN_RENDER_BACKEND_OPENGL
        frontend.getRenderer()->enableAndroidEmulatorGoldfishMitigation(rebuildDrawables);
#else
        EXPECT_FALSE(rebuildDrawables) << "drawables are rebuilt every frame on OpenGL builds only";
#endif
        map.getStyle().loadJSON(motionStyle(colorRelief));
    }

    CameraOptions camera(const LatLng& center, double bearing, double pitch, double zoom = 13.0) const {
        return CameraOptions().withCenter(center).withZoom(zoom).withPitch(pitch).withBearing(bearing);
    }

    void jumpTo(const CameraOptions& options) {
        map.jumpTo(options);
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

    // Runs the loop until the next frame has been drawn (at most 10 s).
    bool nextFrame() {
        const int start = observer.frames;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (observer.frames == start && std::chrono::steady_clock::now() < deadline) {
            loop.runOnce();
        }
        return observer.frames != start;
    }

    PremultipliedImage image() {
        gfx::BackendScope guard{*frontend.getBackend()};
        return frontend.readStillImage();
    }
};

// The share of the frame's pixels below the horizon that are the background's colour. The rows
// above the horizon are left out (none at the cameras used here, but a steeper pitch has them).
double blankShare(const PremultipliedImage& image) {
    size_t blank = 0;
    size_t total = 0;
    for (uint32_t row = 0; row < image.size.height; ++row) {
        for (uint32_t col = 0; col < image.size.width; ++col) {
            const uint8_t* px = image.data.get() + (static_cast<size_t>(row) * image.size.width + col) * 4;
            ++total;
            if (std::abs(px[0] - backgroundRGB[0]) <= 2 && std::abs(px[1] - backgroundRGB[1]) <= 2 &&
                std::abs(px[2] - backgroundRGB[2]) <= 2) {
                ++blank;
            }
        }
    }
    return total ? static_cast<double>(blank) / static_cast<double>(total) : 1.0;
}

// The most of a frame that may be blank. A frame of this view at rest has none (checked first in
// each test); the margin takes a frame's skirt seams and the odd anti-aliased pixel, while a
// tile left blank is at least 1/64th of the view and the reported failures blanked most of it.
constexpr double maxBlankShare = 0.005;

struct MotionResult {
    int frames = 0;
    double worstBlank = 0.0;
    int worstFrame = -1;
};

// Draws every frame until the camera has stopped moving (at most `limit`), reading each one.
MotionResult watchMotion(MotionScene& scene, const std::chrono::milliseconds limit) {
    MotionResult result;
    const auto deadline = std::chrono::steady_clock::now() + limit;
    const int ended = scene.observer.transitionsEnded;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!scene.nextFrame()) {
            break;
        }
        ++result.frames;
        const double share = blankShare(scene.image());
        if (share > result.worstBlank) {
            result.worstBlank = share;
            result.worstFrame = result.frames;
        }
        if (scene.observer.transitionsEnded != ended) {
            break;
        }
    }
    return result;
}

const LatLng start = waypoint(0, 0);

void expectCoveredDuringAFlyLeg(bool rebuildDrawables) {
    for (const double pitch : {60.0, 50.0}) {
        SCOPED_TRACE("pitch " + std::to_string(pitch));
        MotionScene scene(rebuildDrawables);
        scene.jumpTo(scene.camera(start, 0.0, pitch));
        ASSERT_LE(blankShare(scene.image()), maxBlankShare) << "the view at rest is blank: the test proves nothing";

        scene.map.flyTo(scene.camera(waypoint(6, 0), 45.0, pitch), AnimationOptions(Milliseconds(5000)));
        const auto result = watchMotion(scene, std::chrono::seconds(20));
        ASSERT_GE(result.frames, 3) << "too few frames in the flight to test anything";
        EXPECT_LE(result.worstBlank, maxBlankShare)
            << "frame " << result.worstFrame << " of " << result.frames << " left " << result.worstBlank * 100.0
            << " % of the ground blank";
    }
}

void expectCoveredDuringAnEase(bool rebuildDrawables) {
    MotionScene scene(rebuildDrawables);
    scene.jumpTo(scene.camera(start, 0.0, 60.0));
    ASSERT_LE(blankShare(scene.image()), maxBlankShare) << "the view at rest is blank: the test proves nothing";

    scene.map.easeTo(scene.camera(waypoint(6, 0), 45.0, 60.0), AnimationOptions(Milliseconds(5000)));
    const auto result = watchMotion(scene, std::chrono::seconds(20));
    ASSERT_GE(result.frames, 3) << "too few frames in the ease to test anything";
    EXPECT_LE(result.worstBlank, maxBlankShare) << "frame " << result.worstFrame << " of " << result.frames << " left "
                                                << result.worstBlank * 100.0 << " % of the ground blank";
}

// A jump per frame (moveCamera from the app), with no animation: one frame is drawn and read at
// each step, so the frames do not depend on timing.
void expectCoveredDuringAPan(bool rebuildDrawables) {
    MotionScene scene(rebuildDrawables);
    scene.jumpTo(scene.camera(start, 0.0, 60.0));
    ASSERT_LE(blankShare(scene.image()), maxBlankShare) << "the view at rest is blank: the test proves nothing";

    double worst = 0.0;
    int worstStep = -1;
    constexpr int steps = 40;
    for (int step = 1; step <= steps; ++step) {
        const double km = 6.0 * step / steps;
        scene.map.jumpTo(scene.camera(waypoint(km, 0), 45.0 * step / steps, 60.0));
        ASSERT_TRUE(scene.nextFrame());
        const double share = blankShare(scene.image());
        if (share > worst) {
            worst = share;
            worstStep = step;
        }
    }
    EXPECT_LE(worst, maxBlankShare) << "step " << worstStep << " of " << steps << " left " << worst * 100.0
                                    << " % of the ground blank";
}

} // namespace

// The fixture's flight: a fly leg 6 km east at zoom 13, turning to bearing 45, at pitch 60 and 50.
TEST(TerrainMotionDrape, StaysCoveredDuringAFlyLegOverASingleZoomDEM) {
    expectCoveredDuringAFlyLeg(false);
}

// The same leg as an ease: zoom held at 13 all the way.
TEST(TerrainMotionDrape, StaysCoveredDuringAnEaseOverASingleZoomDEM) {
    expectCoveredDuringAnEase(false);
}

// A pure pan at a fixed zoom.
TEST(TerrainMotionDrape, StaysCoveredDuringAPanOverASingleZoomDEM) {
    expectCoveredDuringAPan(false);
}

#if MLN_RENDER_BACKEND_OPENGL
// The same, with every layer's drawables built anew each frame, as on the Android emulator's GL,
// where the Flutter fixture flight was found drawing mostly empty frames. The drape signature
// does not change when drawables are rebuilt over the same tiles and data, so the draped layers'
// tweakers were skipped and the new drawables never got their uniforms; a drape target that
// entered the cover while the camera moved drew them with none, and showed the background.
TEST(TerrainMotionDrape, StaysCoveredDuringAFlyLegWhileDrawablesAreRebuiltEveryFrame) {
    expectCoveredDuringAFlyLeg(true);
}

TEST(TerrainMotionDrape, StaysCoveredDuringAnEaseWhileDrawablesAreRebuiltEveryFrame) {
    expectCoveredDuringAnEase(true);
}

TEST(TerrainMotionDrape, StaysCoveredDuringAPanWhileDrawablesAreRebuiltEveryFrame) {
    expectCoveredDuringAPan(true);
}
#endif
