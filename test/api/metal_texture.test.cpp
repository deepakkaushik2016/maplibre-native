#if MLN_RENDER_BACKEND_METAL

#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/gfx/offscreen_texture.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/mtl/context.hpp>
#include <mln/mtl/headless_backend.hpp>
#include <mln/mtl/renderable_resource.hpp>
#include <mln/mtl/texture2d.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/style/style.hpp>
#include <mln/util/geojson.hpp>
#include <mln/util/run_loop.hpp>

#include <Metal/Metal.hpp>

#include <chrono>
#include <cmath>
#include <exception>
#include <memory>
#include <vector>

using namespace mln;

namespace {

// A heatmap of one point on a white background: an offscreen pass that the main pass samples.
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

// The headless backend hands out its Metal texture for a host to read. A frame is only committed
// to the GPU, not waited for, so the texture must be complete when the call returns, as it was
// when every pass waited. The point moves from one side to the other each frame, and the host
// reads the texture's bytes directly, without readStillImage.
TEST(MetalTexture, TheHeadlessTextureHoldsTheFrameWhenHandedOut) {
    util::RunLoop loop;
    HeadlessFrontend frontend{{512, 256}, 1};
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions()),
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()));
    map.getStyle().loadJSON(heatmapStyle);
    map.jumpTo(CameraOptions().withCenter(LatLng{0.0, 0.0}).withZoom(4.0));
    auto& backend = static_cast<mtl::HeadlessBackend&>(*frontend.getBackend());

    const double lng = 360.0 / std::exp2(4.0) * 128.0 / 512.0;
    for (int frame = 0; frame < 8; ++frame) {
        const bool left = frame % 2 == 0;
        map.getStyle().getSource("point")->as<style::GeoJSONSource>()->setGeoJSON(
            Geometry<double>{mln::Point<double>{left ? -lng : lng, 0.0}});

        bool done = false;
        std::exception_ptr error;
        std::vector<uint8_t> pixels;
        gfx::BackendScope guard{backend};
        map.renderStill([&](const std::exception_ptr& e) {
            error = e;
            if (!e) {
                MTL::Texture* texture = backend.getMetalTexture();
                pixels.resize(static_cast<size_t>(texture->width()) * texture->height() * 4);
                texture->getBytes(pixels.data(),
                                  texture->width() * 4,
                                  MTL::Region::Make2D(0, 0, texture->width(), texture->height()),
                                  0);
            }
            done = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!done && std::chrono::steady_clock::now() < deadline) {
            loop.runOnce();
        }
        ASSERT_TRUE(done && !error) << "still frame " << frame << " did not finish";

        const auto red = [&](uint32_t x) {
            const uint8_t* p = pixels.data() + (static_cast<size_t>(128) * 512 + x) * 4;
            return p[0] > 200 && p[1] < 60 && p[2] < 60;
        };
        EXPECT_TRUE(red(left ? 128 : 384)) << "frame " << frame << ": the point is not drawn where it is";
        EXPECT_FALSE(red(left ? 384 : 128)) << "frame " << frame << ": the point is still drawn where it was";
    }
}

// Offscreen passes are not waited for: a later pass that samples their texture is ordered after
// them by Metal's hazard tracking, which holds for tracked resources, also across command queues.
// Every texture the engine makes, and so every render target's, must be tracked.
TEST(MetalTexture, EngineTexturesAreHazardTracked) {
    mtl::HeadlessBackend backend({64, 64});
    gfx::BackendScope scope{backend};
    auto& context = static_cast<mtl::Context&>(backend.getContext());

    auto target = context.createOffscreenTexture({64, 64}, gfx::TextureChannelDataType::UnsignedByte, true, true);
    target->getResource<mtl::RenderableResource>().bind();
    auto* color = static_cast<mtl::Texture2D&>(*target->getTexture()).getMetalTexture();
    ASSERT_NE(color, nullptr);
    EXPECT_EQ(color->hazardTrackingMode(), MTL::HazardTrackingModeTracked);
    const auto& descriptor = target->getResource<mtl::RenderableResource>().getRenderPassDescriptor();
    ASSERT_NE(descriptor->depthAttachment()->texture(), nullptr);
    EXPECT_EQ(descriptor->depthAttachment()->texture()->hazardTrackingMode(), MTL::HazardTrackingModeTracked);
    target->getResource<mtl::RenderableResource>().swap();

    auto texture = context.createTexture2D();
    texture->setSize({8, 8});
    texture->setFormat(gfx::TexturePixelType::RGBA, gfx::TextureChannelDataType::UnsignedByte);
    texture->create();
    auto* plain = static_cast<mtl::Texture2D&>(*texture).getMetalTexture();
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(plain->hazardTrackingMode(), MTL::HazardTrackingModeTracked);
}

#endif
