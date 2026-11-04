#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/style.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>

using namespace mln;

namespace {

constexpr uint32_t demSize = 256;
constexpr uint32_t viewSize = 512;
// z12 tile (2048, 2048), just south-east of (0, 0): at z12 its 512 px DEM tile fills the 512 px
// frame centred on it.
constexpr int demZoom = 12;
constexpr double tileXY = 2048.0;

// A DEM tile holding a plane: `base` metres plus `slope` metres per pixel eastward and half that
// southward, to the encoding's step (0.1 m Mapbox, 1/256 m Terrarium).
std::string planeTile(bool terrarium, double base, double slope) {
    PremultipliedImage image({demSize, demSize});
    for (uint32_t y = 0; y < demSize; ++y) {
        for (uint32_t x = 0; x < demSize; ++x) {
            const double meters = base + slope * x + 0.5 * slope * y;
            const auto value = static_cast<uint32_t>(terrarium ? std::lround((meters + 32768.0) * 256.0)
                                                               : std::lround((meters + 10000.0) * 10.0));
            uint8_t* px = image.data.get() + (static_cast<size_t>(y) * demSize + x) * 4;
            px[0] = static_cast<uint8_t>(value >> 16);
            px[1] = static_cast<uint8_t>((value >> 8) & 0xff);
            px[2] = static_cast<uint8_t>(value & 0xff);
            px[3] = 255;
        }
    }
    return encodePNG(image);
}

std::string hillshadeStyle(bool terrarium) {
    return R"STYLE({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/{z}-{x}-{y}.png"],
      "encoding": ")STYLE" +
           std::string(terrarium ? "terrarium" : "mapbox") + R"STYLE(",
      "maxzoom": )STYLE" +
           std::to_string(demZoom) + R"STYLE(,
      "tileSize": 512
    }
  },
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "white"}},
    {"id": "hillshade", "type": "hillshade", "source": "dem"}
  ]
})STYLE";
}

// How far each colour channel varies over the middle of the frame, away from the tile's edges
// (every tile is the same plane, so its backfilled border is not the plane's continuation).
std::array<int, 3> shadeSpread(bool terrarium, double base, double slope) {
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    const std::string tile = planeTile(terrarium, base, slope);
    fileSource->tileResponse = [&](const Resource&) {
        Response res;
        res.data = std::make_shared<std::string>(tile);
        return res;
    };
    HeadlessFrontend frontend{{viewSize, viewSize}, 1};
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()));
    map.getStyle().loadJSON(hillshadeStyle(terrarium));
    const double centre = (tileXY + 0.5) / std::pow(2.0, demZoom);
    map.jumpTo(CameraOptions()
                   .withCenter(
                       LatLng{std::atan(std::sinh(M_PI * (1.0 - 2.0 * centre))) * 180.0 / M_PI, centre * 360.0 - 180.0})
                   .withZoom(demZoom));
    const PremultipliedImage image = frontend.render(map).image;
    EXPECT_EQ(image.size, (Size{viewSize, viewSize}));

    std::array<int, 3> lo{{255, 255, 255}};
    std::array<int, 3> hi{{0, 0, 0}};
    for (uint32_t y = 64; y < viewSize - 64; ++y) {
        for (uint32_t x = 64; x < viewSize - 64; ++x) {
            const uint8_t* px = image.data.get() + (static_cast<size_t>(y) * viewSize + x) * 4;
            for (size_t c = 0; c < 3; ++c) {
                lo[c] = std::min<int>(lo[c], px[c]);
                hi[c] = std::max<int>(hi[c], px[c]);
            }
        }
    }
    EXPECT_LT(lo[0], 255) << "no hillshade drawn over the white background";
    return {{hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}};
}

} // namespace

// The hillshade prepare pass decodes the DEM into slopes in a fragment shader. A plane has one
// slope, so every pixel of its hillshade has one shade. If the DEM bytes are not read exactly (a
// GLSL ES sampler left at its default lowp precision may be read in half floats), heights are off
// by part of a byte's step, the slope jumps where a byte carries, and the shade bands.
TEST(Hillshade, APlaneIsShadedEvenly) {
    for (const auto& [terrarium, base, slope] : {std::tuple{false, 3000.0, 1.0}, std::tuple{true, 3000.0, 3.3}}) {
        SCOPED_TRACE(::testing::Message()
                     << (terrarium ? "terrarium" : "mapbox") << ", " << base << " m, " << slope << " m per pixel");
        const auto spread = shadeSpread(terrarium, base, slope);
        for (size_t c = 0; c < 3; ++c) {
            EXPECT_LE(spread[c], 1) << "channel " << c << " varies by " << spread[c] << " over a plane";
        }
    }
}
