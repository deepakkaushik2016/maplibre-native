#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/style.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>

#include <cmath>
#include <cstdint>
#include <string>

using namespace mln;

namespace {

constexpr uint32_t demTileSize = 64;
// DEMData pads the tile with a 2px border on every side, and the texture is the padded image.
constexpr std::size_t demUploadBytes = (demTileSize + 4) * (demTileSize + 4) * 4;

// A Terrain-RGB tile with a gentle ramp, so the color-relief ramp has something to map.
std::string makeDEMTile() {
    PremultipliedImage image({demTileSize, demTileSize});
    for (uint32_t y = 0; y < demTileSize; ++y) {
        for (uint32_t x = 0; x < demTileSize; ++x) {
            uint8_t* px = image.data.get() + (y * demTileSize + x) * 4;
            px[0] = 1;
            px[1] = static_cast<uint8_t>(x * 4);
            px[2] = static_cast<uint8_t>(y * 4);
            px[3] = 255;
        }
    }
    return encodePNG(image);
}

const char* colorReliefStyle = R"STYLE({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/{z}-{x}-{y}.png"],
      "encoding": "mapbox",
      "maxzoom": 0,
      "tileSize": 64
    }
  },
  "layers": [{
    "id": "relief",
    "type": "color-relief",
    "source": "dem",
    "paint": {
      "color-relief-color": [
        "interpolate", ["linear"], ["elevation"],
        -10000, "rgb(0, 0, 255)",
        0, "rgb(0, 255, 0)",
        10000, "rgb(255, 0, 0)"
      ]
    }
  }]
})STYLE";

} // namespace

// The DEM image backs the color-relief draw directly, with no prepare pass, and its pixels only
// change when the tile (re)loads or a neighbour backfills the border. It used to be re-uploaded
// for every visible tile on every frame - a full RGBA re-send of pixels that had not changed.
TEST(ColorRelief, UploadsDEMTextureOncePerTile) {
    util::RunLoop loop;

    auto fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    const std::string tile = makeDEMTile();
    fileSource->tileResponse = [&](const Resource&) {
        Response res;
        res.data = std::make_shared<std::string>(tile);
        return res;
    };

    HeadlessFrontend frontend{{256, 256}, 1};
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()));
    map.getStyle().loadJSON(colorReliefStyle);
    map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(0.0));

    std::vector<std::size_t> uploaded;
    std::size_t previous = 0;
    for (int frame = 0; frame < 4; ++frame) {
        const auto result = frontend.render(map);
        uploaded.push_back(result.stats.textureUpdateBytes - previous);
        previous = result.stats.textureUpdateBytes;
    }

    // Guard against the test passing because the layer never drew: some frame must have paid
    // for the DEM image at least once.
    EXPECT_TRUE(std::any_of(uploaded.begin(), uploaded.end(), [](std::size_t bytes) {
        return bytes >= demUploadBytes;
    })) << "the color-relief layer never uploaded a DEM texture; the test proves nothing";

    // Once the tile is up and nothing has changed, later frames must not re-send it.
    EXPECT_LT(uploaded.back(), demUploadBytes);
}

namespace {

constexpr uint32_t planeSize = 256;

// A Mapbox Terrain-RGB tile holding a plane: 3000 m plus a quarter of a metre per pixel eastward,
// to the 0.1 m step, with DEM pixel i describing the terrain at its centre, i + 0.5.
std::string makePlaneTile() {
    PremultipliedImage image({planeSize, planeSize});
    for (uint32_t y = 0; y < planeSize; ++y) {
        for (uint32_t x = 0; x < planeSize; ++x) {
            const double meters = 3000.0 + 0.25 * x;
            const auto value = static_cast<uint32_t>(std::lround((meters + 10000.0) * 10.0));
            uint8_t* px = image.data.get() + (static_cast<size_t>(y) * planeSize + x) * 4;
            px[0] = static_cast<uint8_t>(value >> 16);
            px[1] = static_cast<uint8_t>((value >> 8) & 0xff);
            px[2] = static_cast<uint8_t>(value & 0xff);
            px[3] = 255;
        }
    }
    return encodePNG(image);
}

// Black at 3000 m to red at 3064 m: four colour steps per metre.
const char* planeReliefStyle = R"STYLE({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/{z}-{x}-{y}.png"],
      "encoding": "mapbox",
      "maxzoom": 0,
      "tileSize": 256
    }
  },
  "layers": [{
    "id": "relief",
    "type": "color-relief",
    "source": "dem",
    "paint": {
      "color-relief-color": [
        "interpolate", ["linear"], ["elevation"],
        3000, "rgb(0, 0, 0)",
        3064, "rgb(255, 0, 0)"
      ]
    }
  }]
})STYLE";

} // namespace

// Each pixel is coloured by the height the DEM holds under it. At 3000 m a half float steps by
// 2 m, eight colour steps on this ramp, so a height decoded or kept in half floats (a GLSL ES
// sampler or float left at its default precision) shows as colour bands up to that far off.
//
// The DEM texture is filtered linearly, which is exact on a plane only with exact filter weights:
// between two texels whose low byte carries, a GPU that filters 8-bit channels with coarse
// weights blends the bytes, not the heights. Those pixels are left out here; they are a
// separate defect (task-18 report, fix round 1).
TEST(ColorRelief, ColoursEachPixelByTheHeightUnderIt) {
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    const std::string tile = makePlaneTile();
    fileSource->tileResponse = [&](const Resource&) {
        Response res;
        res.data = std::make_shared<std::string>(tile);
        return res;
    };
    HeadlessFrontend frontend{{512, 512}, 1};
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()));
    map.getStyle().loadJSON(planeReliefStyle);
    map.jumpTo(CameraOptions().withCenter(LatLng{0, 0}).withZoom(0.0));
    const PremultipliedImage image = frontend.render(map).image;
    ASSERT_EQ(image.size, (Size{512, 512}));

    // The world's one tile fills the frame, a DEM pixel per 2x2 screen pixels. Keep clear of the
    // tile's backfilled border.
    const auto encoded = [](int pixel) {
        return std::lround((3000.0 + 0.25 * pixel + 10000.0) * 10.0);
    };
    double worst = 0.0;
    uint32_t worstX = 0;
    int checked = 0;
    for (uint32_t y = 64; y < 448; y += 8) {
        for (uint32_t x = 64; x < 448; ++x) {
            const double demX = (x + 0.5) / 2.0;
            // The two texels the filter blends; skip a pair whose low byte carries.
            const int left = static_cast<int>(std::floor(demX - 0.5));
            if ((encoded(left) >> 8) != (encoded(left + 1) >> 8)) {
                continue;
            }
            ++checked;
            const double meters = 3000.0 + 0.25 * (demX - 0.5);
            const double red = (meters - 3000.0) / 64.0 * 255.0;
            const uint8_t* px = image.data.get() + (static_cast<size_t>(y) * 512 + x) * 4;
            if (std::abs(px[0] - red) > worst) {
                worst = std::abs(px[0] - red);
                worstX = x;
            }
        }
    }
    ASSERT_GT(checked, 10000);
    EXPECT_LT(worst, 1.5) << "pixel x = " << worstX << " is " << worst << " colour steps off the height under it";
}
