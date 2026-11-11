#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/bound_options.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/style.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace mln;

// The terrain fog (maplibre-gl-js 6.16.0 terrain.fragment.glsl) fades the distance into the sky's
// fog colour. It comes from the style's sky alone, from a pitch of 60° on.

namespace {

std::string seaLevelDEMTile() {
    PremultipliedImage image({256, 256});
    for (uint32_t i = 0; i < 256 * 256; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        // Sea level in Terrain-RGB: (0 + 10000) / 0.1 = 100000 = 1 * 65536 + 134 * 256 + 160
        px[0] = 1;
        px[1] = 134;
        px[2] = 160;
        px[3] = 255;
    }
    return encodePNG(image);
}

// Flat terrain under a background and a fill, and `sky` as the style's sky ("" for none).
std::string fogStyle(const std::string& sky) {
    return std::string(R"STYLE({
  "version": 8,
  "center": [10.0, 46.0],
  "zoom": 13,
  "sources": {
    "dem": {"type": "raster-dem", "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"], "maxzoom": 11, "tileSize": 256},
    "area": {
      "type": "geojson",
      "data": {
        "type": "Feature",
        "properties": {},
        "geometry": {"type": "Polygon", "coordinates": [[[9.9, 46.02], [10.1, 46.02], [10.1, 46.06], [9.9, 46.06], [9.9, 46.02]]]}
      }
    }
  },
  "terrain": {"source": "dem", "exaggeration": 1},
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "#b7c98f"}},
    {"id": "area", "type": "fill", "source": "area", "paint": {"fill-color": "#6f8f4e"}}
  ])STYLE") +
           (sky.empty() ? "" : ",\n  \"sky\": " + sky) + "\n}";
}

// The True Earth sky (outdoor-styles spec section 4).
const std::string trueEarthSky =
    R"({"sky-color": "#3E7CC0", "horizon-color": "#DCE7EF", "fog-color": "#C6D4E0", "sky-horizon-blend": 0.85,
        "horizon-fog-blend": 0.75, "fog-ground-blend": 0.35, "atmosphere-blend": 0.9})";

struct FogScene {
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    std::string dem = seaLevelDEMTile();
    HeadlessFrontend frontend;
    MapAdapter map;

    FogScene(const std::string& sky, double pitch, Size size = {256, 256})
        : frontend(size, 1),
          map(frontend,
              MapObserver::nullObserver(),
              fileSource,
              MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())) {
        fileSource->tileResponse = [this](const Resource&) {
            Response res;
            res.data = std::make_shared<std::string>(dem);
            return res;
        };
        map.setBounds(BoundOptions().withMaxPitch(85));
        map.getStyle().loadJSON(fogStyle(sky));
        map.jumpTo(CameraOptions().withPitch(pitch));
    }

    PremultipliedImage render() { return frontend.render(map).image; }
};

bool sameImage(const PremultipliedImage& a, const PremultipliedImage& b) {
    return a.size == b.size && std::memcmp(a.data.get(), b.data.get(), a.bytes()) == 0;
}

} // namespace

// maplibre-gl-js calculateFogBlendOpacity: no fog below a pitch of 60°. The sky draws above the
// horizon only from about 68.6°, so up to 65° a frame with a sky has no sky in it either, and
// whatever differs is fog.
TEST(TerrainFog, NoFogBelowPitch60) {
    util::RunLoop loop;
    for (const double pitch : {0.0, 50.0, 60.0}) {
        EXPECT_TRUE(sameImage(FogScene(trueEarthSky, pitch).render(), FogScene("", pitch).render()))
            << "pitch " << pitch;
    }
    // The control: at 65° the same sky fogs the distance.
    EXPECT_FALSE(sameImage(FogScene(trueEarthSky, 65).render(), FogScene("", 65).render()));
}

// A style turns the fog off with fog-ground-blend 1: the fog starts at the far plane (as in gl-js).
TEST(TerrainFog, GroundBlendOneTurnsTheFogOff) {
    util::RunLoop loop;
    const std::string noFogSky = R"({"sky-color": "#3E7CC0", "horizon-color": "#DCE7EF", "fog-color": "#C6D4E0",
                                     "fog-ground-blend": 1})";
    EXPECT_TRUE(sameImage(FogScene(noFogSky, 65).render(), FogScene("", 65).render()));
}

// Frame time of the same still frame of terrain, rendered over and over with the True Earth sky and
// without a sky, interleaved so both sides see the same GPU clocks. Each sample is renderFrame()
// and the read-back that waits for the GPU to finish it, so frames are not capped by a display. A
// measurement to read, not a gate: run with --gtest_also_run_disabled_tests. At pitch 50 and 60
// gl-js draws no fog (the shader only tests the opacity); at 75 the sky and full fog draw.
TEST(TerrainFog, DISABLED_FrameTime) {
    util::RunLoop loop;
    constexpr int warmUp = 50;
    constexpr int samples = 500;
    const Size size{1024, 1024};

    for (const double pitch : {50.0, 60.0, 75.0}) {
        FogScene without("", pitch, size);
        FogScene with(trueEarthSky, pitch, size);
        without.render();
        with.render();

        std::vector<double> times[2];
        for (int i = 0; i < warmUp + samples; ++i) {
            for (int side = 0; side < 2; ++side) {
                auto& frontend = side == 0 ? without.frontend : with.frontend;
                const auto start = std::chrono::steady_clock::now();
                frontend.renderFrame();
                frontend.readStillImage();
                const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
                if (i >= warmUp) {
                    times[side].push_back(elapsed.count());
                }
            }
        }
        double medians[2];
        for (int side = 0; side < 2; ++side) {
            std::ranges::sort(times[side]);
            medians[side] = times[side][times[side].size() / 2];
        }
        std::printf(
            "terrain fog frame time at pitch %.0f, %ux%u, median of %d: without sky %.4f ms, with fog %.4f ms, delta "
            "%+.4f ms\n",
            pitch,
            size.width,
            size.height,
            samples,
            medians[0],
            medians[1],
            medians[1] - medians[0]);
    }
}
