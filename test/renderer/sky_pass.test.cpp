#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/bound_options.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/style.hpp>
#include <mln/util/run_loop.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace mln;

// The sky pass (RenderSky) draws one quad above the horizon line, before everything else, and
// nothing at all when no part of the viewport is above the horizon. These tests count the frame's
// draw calls with and without a sky in the style.

namespace {

std::string skyStyle(bool withSky) {
    return std::string(R"STYLE({
  "version": 8,
  "center": [35.38, 31.55],
  "zoom": 15,
  "sources": {
    "area": {
      "type": "geojson",
      "data": {
        "type": "Feature",
        "properties": {},
        "geometry": {"type": "Polygon", "coordinates": [[[35.3, 31.5], [35.46, 31.5], [35.46, 31.6], [35.3, 31.6], [35.3, 31.5]]]}
      }
    }
  },
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "#ccaa83"}},
    {"id": "area", "type": "fill", "source": "area", "paint": {"fill-color": "#3a7", "fill-opacity": 0.5}}
  ])STYLE") +
           (withSky ? R"STYLE(,
  "sky": {"sky-color": "#3E7CC0", "horizon-color": "#DCE7EF", "sky-horizon-blend": 0.85})STYLE"
                    : "") +
           "\n}";
}

struct SkyScene {
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    HeadlessFrontend frontend;
    MapAdapter map;

    SkyScene(bool withSky, double pitch, Size size = {256, 256})
        : frontend(size, 1),
          map(frontend,
              MapObserver::nullObserver(),
              fileSource,
              MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())) {
        map.setBounds(BoundOptions().withMaxPitch(85));
        map.getStyle().loadJSON(skyStyle(withSky));
        map.jumpTo(CameraOptions().withPitch(pitch));
    }

    int drawCalls() { return frontend.render(map).stats.numDrawCalls; }
};

} // namespace

TEST(SkyPass, NoDrawWhenTheHorizonIsOffScreen) {
    util::RunLoop loop;

    // Pitch 30: the horizon is far above the top of the viewport.
    const int withoutSky = SkyScene(false, 30).drawCalls();
    ASSERT_GT(withoutSky, 0);
    EXPECT_EQ(SkyScene(true, 30).drawCalls(), withoutSky);

    // Pitch 80, the control: the same count would have passed above even if the sky drew nothing.
    EXPECT_EQ(SkyScene(true, 80).drawCalls(), SkyScene(false, 80).drawCalls() + 1);
}

// Frame time of the same still frame, rendered over and over with and without a sky, interleaved
// so both sides see the same GPU clocks. Each sample is renderFrame() and the read-back that waits
// for the GPU to finish it, so frames are not capped by a display. A measurement to read, not a
// gate: run with --gtest_also_run_disabled_tests.
TEST(SkyPass, DISABLED_FrameTime) {
    util::RunLoop loop;
    constexpr int warmUp = 50;
    constexpr int samples = 500;
    const Size size{1024, 1024};

    for (const double pitch : {30.0, 80.0}) {
        SkyScene without(false, pitch, size);
        SkyScene with(true, pitch, size);
        without.drawCalls();
        with.drawCalls();

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
            "sky frame time at pitch %.0f, %ux%u, median of %d: without %.4f ms, with %.4f ms, delta %+.4f ms\n",
            pitch,
            size.width,
            size.height,
            samples,
            medians[0],
            medians[1],
            medians[1] - medians[0]);
    }
}
