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
#include <climits>
#include <cstdlib>
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

// `bounds`: the DEM source's tileset bounds as a JSON array, or empty for none.
std::string style(const std::string& bounds = "") {
    return R"STYLE({
  "version": 8,
  "sources": {
    "dem": {"type": "raster-dem", "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"], "encoding": "terrarium",
            "minzoom": )STYLE" +
           std::to_string(demZoom) + R"STYLE(, "maxzoom": )STYLE" + std::to_string(demZoom) +
           R"STYLE(, "tileSize": 256)STYLE" + (bounds.empty() ? std::string() : ", \"bounds\": " + bounds) +
           R"STYLE(},
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
// loading, as over a slow network) until moved to `empty`, which answers them 204 No Content, as
// a sparse tileset does where it has no data; every other one arrives at once.
struct Scene {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    HeadlessFrontend frontend{{viewWidth, viewHeight}, 1};
    Observer observer;
    MapAdapter map{
        frontend, observer, fileSource, MapOptions().withMapMode(MapMode::Continuous).withSize(frontend.getSize())};
    std::set<CanonicalTileID> withheld;
    std::set<CanonicalTileID> empty;
    /// Every tile of this row and south of it answers empty too.
    uint32_t emptyFromRow = UINT32_MAX;
    std::set<CanonicalTileID> served;
    const std::string tile = plateauTile();

    explicit Scene(std::set<CanonicalTileID> withheld_,
                   std::set<CanonicalTileID> empty_ = {},
                   const std::string& bounds = "",
                   uint32_t emptyFromRow_ = UINT32_MAX)
        : withheld(std::move(withheld_)),
          empty(std::move(empty_)),
          emptyFromRow(emptyFromRow_) {
        fileSource->tileResponse = [this](const Resource& resource) -> std::optional<Response> {
            Response res;
            if (!resource.tileData) {
                res.noContent = true;
                return res;
            }
            const CanonicalTileID id(
                resource.tileData->z, static_cast<uint32_t>(resource.tileData->x), resource.tileData->y);
            if (empty.contains(id) || id.y >= emptyFromRow) {
                res.noContent = true;
                return res;
            }
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
        map.getStyle().loadJSON(style(bounds));
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

// The share of pixels that differ between two frames by more than a little in any channel.
double differingShare(const PremultipliedImage& a, const PremultipliedImage& b) {
    if (a.size != b.size) return 1.0;
    size_t differing = 0;
    const size_t count = static_cast<size_t>(a.size.width) * a.size.height;
    for (size_t i = 0; i < count; ++i) {
        for (int c = 0; c < 3; ++c) {
            if (std::abs(int(a.data[i * 4 + c]) - int(b.data[i * 4 + c])) > 8) {
                ++differing;
                break;
            }
        }
    }
    return static_cast<double>(differing) / static_cast<double>(count);
}

// The DEM tiles under the bottom of the frame: under the eye's ground and under the bottom edge's
// sea-level point, between which the line of sight meets the plateau.
std::set<CanonicalTileID> nearTiles(Scene& scene) {
    std::set<CanonicalTileID> near;
    if (const auto eye = scene.map.getFreeCameraOptions().getLocation()) {
        near.insert(demTileAt(eye->location));
    }
    for (const double x : {0.0, viewWidth / 2.0, static_cast<double>(viewWidth)}) {
        near.insert(demTileAt(scene.map.latLngForPixel({x, static_cast<double>(viewHeight)})));
    }
    return near;
}

} // namespace

TEST(TerrainMissingDEM, NearGroundWithNoDEMYetIsNotDrawnBelowItsNeighbours) {
    // Every DEM tile loaded: stripes all the way down.
    std::set<CanonicalTileID> near;
    {
        Scene loaded({});
        const auto still = loaded.image();
        ASSERT_LE(tallestRun(still), maxRun) << "the loaded view has no stripes: the test proves nothing";
        near = nearTiles(loaded);
        ASSERT_FALSE(near.empty());
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

namespace {

// The near DEM tiles of the plateau view, from a scene with every tile loaded.
std::set<CanonicalTileID> plateauNearTiles() {
    Scene loaded({});
    return nearTiles(loaded);
}

} // namespace

// Where the source has no DEM (it answers 204), a tile has nothing to wait for and is drawn flat
// at sea level, as before - even when, as in a continuous map, the empty answer lands after the
// tile was first drawn on a neighbour's heights while its request was out. The frame then must
// match that of a map that had the empty answers from the start.
TEST(TerrainMissingDEM, ATileBorrowingHeightsGoesFlatOnceItsDEMAnswersEmpty) {
    const auto near = plateauNearTiles();
    ASSERT_FALSE(near.empty());

    PremultipliedImage emptyFromTheStart;
    {
        Scene scene({}, near);
        emptyFromTheStart = scene.image();
        ASSERT_GT(tallestRun(emptyFromTheStart), maxRun)
            << "the empty tiles are not drawn below their neighbours: the test proves nothing";
    }

    Scene scene(near);
    const auto borrowing = scene.image();
    ASSERT_LE(tallestRun(borrowing), maxRun) << "the loading tiles do not borrow heights: the test proves nothing";

    // The requests still out now answer empty.
    scene.empty = near;
    scene.settle();
    const auto still = scene.image();
    EXPECT_LE(differingShare(still, emptyFromTheStart), 0.01)
        << "the tile kept the heights it borrowed after its DEM answered empty";
}

// Past the DEM tileset's bounds the source asks for nothing, so no answer, empty or not, ever
// comes: those tiles are drawn flat at sea level as before, not on a neighbour's heights.
TEST(TerrainMissingDEM, ATileOutsideTheTilesetBoundsDoesNotBorrowHeights) {
    const auto near = plateauNearTiles();
    ASSERT_FALSE(near.empty());
    // The bounds end a hair north of the northern edge of the southernmost near tile row, so the
    // tiles south of that edge (the near ones; the view looks north) lie outside.
    uint32_t nearRow = 0;
    for (const auto& id : near) nearRow = std::max(nearRow, id.y);
    uint32_t farRow = nearRow;
    for (const auto& id : near) farRow = std::min(farRow, id.y);
    const double n = std::pow(2.0, demZoom);
    const double edge = util::rad2deg(std::atan(std::sinh(M_PI * (1.0 - 2.0 * farRow / n))));
    const std::string bounds = "[10.0, " + std::to_string(edge + 0.0001) + ", 13.0, 49.0]";

    PremultipliedImage emptyFromTheStart;
    {
        Scene scene({}, {}, "", farRow);
        emptyFromTheStart = scene.image();
        ASSERT_GT(tallestRun(emptyFromTheStart), maxRun)
            << "the empty tiles are not drawn below their neighbours: the test proves nothing";
    }
    Scene scene({}, {}, bounds);
    for (const auto& id : near) {
        EXPECT_FALSE(scene.served.contains(id)) << "a near tile was asked for: the bounds miss it";
    }
    const auto still = scene.image();
    EXPECT_LE(differingShare(still, emptyFromTheStart), 0.01)
        << "a tile outside the tileset's bounds borrowed a neighbour's heights";
}
