#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/style/style.hpp>
#include <mln/util/geojson.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>

#include <chrono>
#include <exception>
#include <memory>

using namespace mln;

namespace {

// A heatmap of one point on a white background. The heatmap is drawn into an offscreen texture
// that the main pass then samples, so a frame is two passes the GPU runs in order.
const char* heatmapStyle = R"STYLE({
  "version": 8,
  "sources": {"point": {"type": "geojson", "data": {"type": "FeatureCollection", "features": []}}},
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "white"}},
    {"id": "heat", "type": "heatmap", "source": "point",
     "paint": {"heatmap-radius": 60, "heatmap-intensity": 4, "heatmap-opacity": 1,
               "heatmap-color": ["interpolate", ["linear"], ["heatmap-density"],
                                 0, "rgba(0, 0, 0, 0)", 0.1, "#f00", 1, "#f00"]}}
  ]
})STYLE";

} // namespace

// A still image read as soon as its frame is done holds that frame. The frame and its offscreen
// passes are only committed to the GPU, not waited for, so the read must wait for them; a read
// before the GPU finished shows the previous frame. The point moves from one side to the other
// each frame, so a stale read fails whichever frame it happens in.
TEST(API, AStillImageReadRightAfterTheFrameHoldsThatFrame) {
    util::RunLoop loop;
    HeadlessFrontend frontend{{512, 256}, 1};
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions()),
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()));
    map.getStyle().loadJSON(heatmapStyle);
    map.jumpTo(CameraOptions().withCenter(LatLng{0.0, 0.0}).withZoom(4.0));

    PremultipliedImage image;
    const auto renderStill = [&] {
        bool done = false;
        std::exception_ptr error;
        gfx::BackendScope guard{*frontend.getBackend()};
        map.renderStill([&](const std::exception_ptr& e) {
            error = e;
            if (!e) {
                image = frontend.readStillImage();
            }
            done = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!done && std::chrono::steady_clock::now() < deadline) {
            loop.runOnce();
        }
        return done && !error;
    };
    const auto red = [&](uint32_t x, uint32_t y) {
        const uint8_t* p = image.data.get() + (static_cast<size_t>(y) * image.size.width + x) * 4;
        return p[0] > 200 && p[1] < 60 && p[2] < 60;
    };

    // The point sits at the centre of the left or the right half of the view.
    const double lng = 360.0 / std::exp2(4.0) * 128.0 / 512.0;
    for (int frame = 0; frame < 8; ++frame) {
        const bool left = frame % 2 == 0;
        map.getStyle().getSource("point")->as<style::GeoJSONSource>()->setGeoJSON(
            Geometry<double>{Point<double>{left ? -lng : lng, 0.0}});
        ASSERT_TRUE(renderStill()) << "still frame " << frame << " did not finish";
        const uint32_t here = left ? 128 : 384;
        const uint32_t there = left ? 384 : 128;
        EXPECT_TRUE(red(here, 128)) << "frame " << frame << ": the point is not drawn where it is";
        EXPECT_FALSE(red(there, 128)) << "frame " << frame << ": the point is still drawn where it was";
    }
}
