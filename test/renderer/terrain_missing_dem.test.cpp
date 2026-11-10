#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_observer.hpp>
#include <mln/map/map_options.hpp>
#include <mln/storage/response.hpp>
#include <mln/style/style.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/geo.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>

using namespace mln;

// A terrain mesh tile whose DEM has not arrived, with no ancestor DEM loaded either, is drawn
// while it streams. It used to be drawn flat at sea level: on high ground (a 2.5 km plateau
// here, Yosemite's rim in the app) the tile sank kilometres below its loaded neighbours, and
// the camera looked down their skirts, which smear the drape straight down the screen as
// "walls". It now takes its height from the edge of a loaded neighbour's DEM, so the ground
// near the camera stays ground.
//
// The ground is drawn as east-west stripes, red and blue, about 110 m wide, over a white
// background. Where the ground is drawn as ground, a column of pixels goes from stripe to stripe
// down the frame, none taller than the nearest stripe; down a skirt it stays in one colour for as
// long as the skirt is tall, as every row of a skirt samples the same drape row, and where the
// ground is left undrawn it shows the background.

namespace {

constexpr uint32_t viewWidth = 411;
constexpr uint32_t viewHeight = 340;
constexpr int demZoom = 12;
constexpr uint32_t demSize = 256;
constexpr double plateau = 2500.0;
constexpr double exaggeration = 1.25;
const LatLng center{47.20, 11.40};
constexpr double stripeDegrees = 0.001;
// Taller than any one stripe of the loaded view, near the camera included.
constexpr uint32_t maxRun = 40;

std::string plateauTile() {
    PremultipliedImage image({demSize, demSize});
    const double value = plateau + 32768.0;
    const auto whole = static_cast<uint32_t>(std::floor(value));
    for (uint32_t i = 0; i < demSize * demSize; ++i) {
        uint8_t* px = image.data.get() + static_cast<size_t>(i) * 4;
        px[0] = static_cast<uint8_t>(whole >> 8);
        px[1] = static_cast<uint8_t>(whole & 0xFF);
        px[2] = 0;
        px[3] = 255;
    }
    return encodePNG(image);
}

std::string stripes() {
    std::string features;
    for (int i = 0; i < 400; ++i) {
        const double south = center.latitude() - 0.2 + i * stripeDegrees;
        const double north = south + stripeDegrees;
        const std::string s = std::to_string(south);
        const std::string n = std::to_string(north);
        if (!features.empty()) features += ",";
        features += R"({"type":"Feature","properties":{"red":)" + std::string(i % 2 ? "true" : "false") +
                    R"(},"geometry":{"type":"Polygon","coordinates":[[[11.0,)" + s + "],[11.8," + s + "],[11.8," + n +
                    "],[11.0," + n + "],[11.0," + s + "]]]}}";
    }
    return R"({"type":"FeatureCollection","features":[)" + features + "]}";
}

std::string style() {
    return R"STYLE({
  "version": 8,
  "sources": {
    "dem": {"type": "raster-dem", "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"], "encoding": "terrarium",
            "minzoom": )STYLE" +
           std::to_string(demZoom) + R"STYLE(, "maxzoom": )STYLE" + std::to_string(demZoom) +
           R"STYLE(, "tileSize": 256},
    "stripes": {"type": "geojson", "data": )STYLE" +
           stripes() + R"STYLE(}
  },
  "terrain": {"source": "dem", "exaggeration": )STYLE" +
           std::to_string(exaggeration) + R"STYLE(},
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "#ffffff"}},
    {"id": "stripes", "type": "fill", "source": "stripes",
     "paint": {"fill-color": ["case", ["get", "red"], "#ff0000", "#0000ff"], "fill-antialias": false}}
  ]
})STYLE";
}

class Observer : public MapObserver {
public:
    void onDidFinishRenderingFrame(const RenderFrameStatus& status) override {
        ++frames;
        idle = !status.needsRepaint;
    }
    int frames = 0;
    bool idle = false;
};

CanonicalTileID demTileAt(const LatLng& point) {
    const double n = std::pow(2.0, demZoom);
    const double x = (point.longitude() + 180.0) / 360.0 * n;
    const double latRad = util::deg2rad(point.latitude());
    const double y = (1.0 - std::log(std::tan(latRad) + 1.0 / std::cos(latRad)) / M_PI) / 2.0 * n;
    return {static_cast<uint8_t>(demZoom), static_cast<uint32_t>(x), static_cast<uint32_t>(y)};
}

// A phone-sized pitched map over the plateau. DEM tiles in `withheld` never arrive (they stay
// loading, as over a slow network); every other one does at once.
struct Scene {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    HeadlessFrontend frontend{{viewWidth, viewHeight}, 1};
    Observer observer;
    MapAdapter map{
        frontend, observer, fileSource, MapOptions().withMapMode(MapMode::Continuous).withSize(frontend.getSize())};
    std::set<CanonicalTileID> withheld;
    std::set<CanonicalTileID> served;
    const std::string tile = plateauTile();

    explicit Scene(std::set<CanonicalTileID> withheld_)
        : withheld(std::move(withheld_)) {
        fileSource->tileResponse = [this](const Resource& resource) -> std::optional<Response> {
            Response res;
            if (!resource.tileData) {
                res.noContent = true;
                return res;
            }
            const CanonicalTileID id(
                resource.tileData->z, static_cast<uint32_t>(resource.tileData->x), resource.tileData->y);
            if (withheld.contains(id)) {
                return std::nullopt;
            }
            if (id.z != demZoom) {
                res.error = std::make_unique<Response::Error>(Response::Error::Reason::NotFound, "no such zoom");
                return res;
            }
            served.insert(id);
            res.data = std::make_shared<std::string>(tile);
            return res;
        };
        map.getStyle().loadJSON(style());
        map.jumpTo(CameraOptions().withCenter(center).withZoom(13.0).withPitch(50.0).withCenterAltitude(plateau *
                                                                                                        exaggeration));
        settle();
    }

    // Draws frames until the map is idle with nothing left to draw, or for 300 frames: a view
    // with a DEM tile still loading never goes idle (at most 20 s either way).
    void settle() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        int quiet = 0;
        for (int frame = 0; frame < 300 && std::chrono::steady_clock::now() < deadline && quiet < 10; ++frame) {
            const int start = observer.frames;
            map.triggerRepaint();
            while (observer.frames == start && std::chrono::steady_clock::now() < deadline) {
                loop.runOnce();
            }
            quiet = observer.idle ? quiet + 1 : 0;
        }
    }

    PremultipliedImage image() {
        gfx::BackendScope guard{*frontend.getBackend()};
        return frontend.readStillImage();
    }
};

enum class Stripe {
    None,
    Red,
    Blue
};

Stripe stripeAt(const PremultipliedImage& image, uint32_t col, uint32_t row) {
    const uint8_t* px = image.data.get() + (static_cast<size_t>(row) * image.size.width + col) * 4;
    if (px[0] > 160 && px[1] < 96 && px[2] < 96) return Stripe::Red;
    if (px[2] > 160 && px[0] < 96 && px[1] < 96) return Stripe::Blue;
    return Stripe::None;
}

// The tallest run of rows, in any of five columns, that stays in one stripe or shows the
// background (ground left undrawn) - a skirt's smear or a hole.
uint32_t tallestRun(const PremultipliedImage& image) {
    uint32_t tallest = 0;
    for (const double share : {0.1, 0.3, 0.5, 0.7, 0.9}) {
        const auto col = static_cast<uint32_t>(share * (image.size.width - 1));
        uint32_t run = 0;
        Stripe last = Stripe::None;
        for (uint32_t row = 0; row < image.size.height; ++row) {
            const Stripe now = stripeAt(image, col, row);
            run = (row > 0 && now == last) ? run + 1 : 1;
            last = now;
            tallest = std::max(tallest, run);
        }
    }
    return tallest;
}

} // namespace

TEST(TerrainMissingDEM, NearGroundWithNoDEMYetIsNotDrawnBelowItsNeighbours) {
    // Every DEM tile loaded: stripes all the way down.
    std::set<CanonicalTileID> near;
    {
        Scene loaded({});
        const auto still = loaded.image();
        ASSERT_LE(tallestRun(still), maxRun) << "the loaded view has no stripes: the test proves nothing";
        // The DEM tiles under the bottom of the frame: under the eye's ground and under the
        // bottom edge's sea-level point, between which the line of sight meets the plateau.
        const auto eye = loaded.map.getFreeCameraOptions().getLocation();
        ASSERT_TRUE(eye);
        near.insert(demTileAt(eye->location));
        for (const double x : {0.0, viewWidth / 2.0, static_cast<double>(viewWidth)}) {
            near.insert(demTileAt(loaded.map.latLngForPixel({x, static_cast<double>(viewHeight)})));
        }
        for (const auto& id : near) {
            ASSERT_TRUE(loaded.served.contains(id)) << "the near DEM tile is not in the cover";
        }
    }

    // The same view with the DEM under the near ground still loading (and no ancestor of it, as
    // the source has one zoom only).
    Scene streaming(near);
    const auto still = streaming.image();
    EXPECT_LE(tallestRun(still), maxRun) << "the ground near the camera is drawn as walls";
}
