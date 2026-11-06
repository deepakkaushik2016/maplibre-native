#if MLN_RENDER_BACKEND_METAL

#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/gfx/offscreen_texture.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/gfx/command_encoder.hpp>
#include <mln/gfx/render_pass.hpp>
#include <mln/gfx/rendering_stats.hpp>
#include <mln/mtl/buffer_resource.hpp>
#include <mln/mtl/context.hpp>
#include <mln/mtl/headless_backend.hpp>
#include <mln/mtl/render_pass.hpp>
#include <mln/mtl/renderable_resource.hpp>
#include <mln/mtl/texture2d.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/style/style.hpp>
#include <mln/util/geojson.hpp>
#include <mln/util/run_loop.hpp>

#include <Metal/Metal.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <memory>
#include <thread>
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
// them by Metal's hazard tracking, which holds for tracked resources submitted to one command queue
// (across queues it takes an event; see TheFrameAndOffscreenPassesShareTheBackendQueue). Every
// texture the engine makes, and so every render target's colour, depth and stencil, must be tracked.
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
    ASSERT_NE(descriptor->stencilAttachment()->texture(), nullptr);
    EXPECT_EQ(descriptor->stencilAttachment()->texture()->hazardTrackingMode(), MTL::HazardTrackingModeTracked);
    target->getResource<mtl::RenderableResource>().swap();

    auto texture = context.createTexture2D();
    texture->setSize({8, 8});
    texture->setFormat(gfx::TexturePixelType::RGBA, gfx::TextureChannelDataType::UnsignedByte);
    texture->create();
    auto* plain = static_cast<mtl::Texture2D&>(*texture).getMetalTexture();
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(plain->hazardTrackingMode(), MTL::HazardTrackingModeTracked);
}

// Metal orders a pass after an unwaited one through hazard tracking only within one command
// queue; across queues it takes an event. So the frame and every offscreen pass it samples must
// commit on the renderer backend's queue. Hosts with their own renderable (MLNMapView, GLFW) take
// that queue too.
TEST(MetalTexture, TheFrameAndOffscreenPassesShareTheBackendQueue) {
    mtl::HeadlessBackend backend({64, 64});
    gfx::BackendScope scope{backend};
    auto& context = static_cast<mtl::Context&>(backend.getContext());
    const auto* queue = backend.getCommandQueue().get();
    ASSERT_NE(queue, nullptr);

    auto target = context.createOffscreenTexture({64, 64}, gfx::TextureChannelDataType::UnsignedByte, true, false);
    auto& targetResource = target->getResource<mtl::RenderableResource>();
    targetResource.bind();
    EXPECT_EQ(targetResource.getCommandBuffer()->commandQueue(), queue) << "an offscreen pass";
    targetResource.swap();

    auto& frame = backend.getDefaultRenderable().getResource<mtl::RenderableResource>();
    frame.bind();
    EXPECT_EQ(frame.getCommandBuffer()->commandQueue(), queue) << "the frame";
    frame.swap();
}

namespace {

constexpr uint32_t texels = 8;
constexpr NS::UInteger texelBytes = texels * texels * 4;

std::vector<uint8_t> filled(uint8_t value) {
    return std::vector<uint8_t>(texelBytes, value);
}

std::shared_ptr<mtl::Texture2D> makeTexture(mtl::Context& context, uint8_t value) {
    auto texture = std::static_pointer_cast<mtl::Texture2D>(context.createTexture2D());
    texture->setFormat(gfx::TexturePixelType::RGBA, gfx::TextureChannelDataType::UnsignedByte);
    const auto pixels = filled(value);
    texture->upload(pixels.data(), {texels, texels});
    return texture;
}

// What a frame the CPU has not waited for does with a resource: a render pass binds it, and the
// GPU reads it later. The read is held back by `gate`, as a slow GPU would hold it, and copies the
// resource into `readback` for the case to check.
struct GatedRead {
    mtl::MTLBufferPtr readback;
    mtl::MTLCommandBufferPtr commandBuffer;
};

GatedRead commitGatedRead(mtl::Context& context,
                          gfx::OffscreenTexture& target,
                          MTL::SharedEvent* gate,
                          mtl::Texture2D* texture,
                          const mtl::BufferResource* buffer = nullptr) {
    const auto& device = context.getBackend().getDevice();
    GatedRead read{.readback = NS::TransferPtr(device->newBuffer(texelBytes, MTL::ResourceStorageModeShared)),
                   .commandBuffer = {}};
    auto encoder = context.createCommandEncoder();
    MTL::Texture* readTexture = nullptr;
    MTL::Buffer* readBuffer = nullptr;
    {
        auto pass = encoder->createRenderPass("gated read", {target, Color::white(), {}, {}});
        auto& renderPass = static_cast<mtl::RenderPass&>(*pass);
        if (texture) {
            texture->bind(renderPass, 0);
            texture->unbind(renderPass, 0);
            readTexture = texture->getMetalTexture();
        }
        if (buffer) {
            renderPass.bindVertex(*buffer, 0, 1);
            readBuffer = buffer->getMetalBuffer().get();
        }
        read.commandBuffer = renderPass.getCommandBuffer();
    }
    read.commandBuffer->encodeWait(gate, 1);
    auto* blit = read.commandBuffer->blitCommandEncoder();
    if (readTexture) {
        blit->copyFromTexture(readTexture,
                              0,
                              0,
                              MTL::Origin::Make(0, 0, 0),
                              MTL::Size::Make(texels, texels, 1),
                              read.readback.get(),
                              0,
                              texels * 4,
                              texelBytes);
    } else if (readBuffer) {
        blit->copyFromBuffer(readBuffer, 0, read.readback.get(), 0, texelBytes);
    }
    blit->endEncoding();
    encoder->present(target);
    return read;
}

std::vector<uint8_t> finish(const GatedRead& read) {
    read.commandBuffer->waitUntilCompleted();
    const auto* bytes = static_cast<const uint8_t*>(read.readback->contents());
    return {bytes, bytes + texelBytes};
}

struct GPUFixture {
    mtl::HeadlessBackend backend{{texels, texels}};
    gfx::BackendScope scope{backend};
    mtl::Context& context = static_cast<mtl::Context&>(backend.getContext());
    std::unique_ptr<gfx::OffscreenTexture> target = context.createOffscreenTexture(
        {texels, texels}, gfx::TextureChannelDataType::UnsignedByte, false, false);
    NS::SharedPtr<MTL::SharedEvent> gate = NS::TransferPtr(context.getBackend().getDevice()->newSharedEvent());

    // Opens the gate from another thread after `delay`, as the GPU would finish, while this one may
    // be waiting. It stops early once the case has opened the gate itself.
    std::thread openGateLater(std::chrono::milliseconds delay) {
        return std::thread([event = gate, delay] {
            const auto deadline = std::chrono::steady_clock::now() + delay;
            while (event->signaledValue() == 0 && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            event->setSignaledValue(1);
        });
    }
};

} // namespace

// A frame in flight still reads a texture that the next frame uploads again whole. The upload must
// not overwrite the texels under it: the in-flight frame keeps what it was encoded with, and the
// CPU does not wait for it (the upload goes to a texture of its own).
TEST(MetalTexture, AnUploadKeepsTheTexelsAFrameInFlightStillReads) {
    GPUFixture gpu;
    auto texture = makeTexture(gpu.context, 0x11);
    const auto stats = gpu.context.renderingStats();

    const auto read = commitGatedRead(gpu.context, *gpu.target, gpu.gate.get(), texture.get());
    const auto replacement = filled(0x22);
    texture->upload(replacement.data(), {texels, texels});
    EXPECT_EQ(gpu.gate->signaledValue(), 0u) << "the upload waited for the GPU";
    gpu.gate->setSignaledValue(1);

    EXPECT_EQ(finish(read), filled(0x11)) << "the upload overwrote texels a frame in flight reads";
    EXPECT_EQ(gpu.context.renderingStats().numTextureUploadWaits, stats.numTextureUploadWaits);
    EXPECT_EQ(gpu.context.renderingStats().numTexturesReplacedInUse, stats.numTexturesReplacedInUse + 1);

    // The texture holds the upload for the next frame to draw.
    auto next = commitGatedRead(gpu.context, *gpu.target, gpu.gate.get(), texture.get());
    EXPECT_EQ(finish(next), filled(0x22));
}

// A sub-region write keeps the rest of the texture (an icon atlas patch), so it can't go to a new
// texture: it waits until the frames that read the texture are done, and only those.
TEST(MetalTexture, ASubRegionWriteWaitsForAFrameInFlightThatReadsTheTexture) {
    GPUFixture gpu;
    auto texture = makeTexture(gpu.context, 0x11);
    const auto stats = gpu.context.renderingStats();

    const auto read = commitGatedRead(gpu.context, *gpu.target, gpu.gate.get(), texture.get());
    auto opener = gpu.openGateLater(std::chrono::milliseconds(200));
    const auto patch = filled(0x33);
    texture->uploadSubRegion(patch.data(), {texels, texels}, 0, 0);
    opener.join();

    EXPECT_EQ(finish(read), filled(0x11)) << "the write overwrote texels a frame in flight reads";
    EXPECT_EQ(gpu.context.renderingStats().numTextureUploadWaits, stats.numTextureUploadWaits + 1);

    auto next = commitGatedRead(gpu.context, *gpu.target, gpu.gate.get(), texture.get());
    EXPECT_EQ(finish(next), filled(0x33));
}

// The common frame writes only textures no frame in flight reads: new ones, ones the last frames
// did not draw, and ones bound in the frame being encoded, which has not been committed. None of
// these may wait, even while another frame is held on the GPU.
TEST(MetalTexture, AWriteToATextureNoFrameInFlightReadsDoesNotWait) {
    GPUFixture gpu;
    auto drawn = makeTexture(gpu.context, 0x11);
    auto earlier = makeTexture(gpu.context, 0x11);
    auto encoding = makeTexture(gpu.context, 0x11);
    auto unused = makeTexture(gpu.context, 0x11);

    // `earlier` was read by a frame that has finished.
    auto finished = NS::TransferPtr(gpu.context.getBackend().getDevice()->newSharedEvent());
    finished->setSignaledValue(1);
    finish(commitGatedRead(gpu.context, *gpu.target, finished.get(), earlier.get()));

    // Another frame, which reads only `drawn`, is held on the GPU. A wait for it would block here
    // until the opener releases it.
    const auto read = commitGatedRead(gpu.context, *gpu.target, gpu.gate.get(), drawn.get());
    auto opener = gpu.openGateLater(std::chrono::milliseconds(5000));
    const auto stats = gpu.context.renderingStats();

    // `encoding` is bound in a pass whose command buffer has not been committed.
    auto encoder = gpu.context.createCommandEncoder();
    auto pass = encoder->createRenderPass("encoding", {*gpu.target, Color::white(), {}, {}});
    encoding->bind(static_cast<mtl::RenderPass&>(*pass), 0);
    encoding->unbind(static_cast<mtl::RenderPass&>(*pass), 0);

    const auto patch = filled(0x44);
    for (auto* texture : {earlier.get(), encoding.get(), unused.get()}) {
        texture->uploadSubRegion(patch.data(), {texels, texels}, 0, 0);
        texture->upload(patch.data(), {texels, texels});
    }
    EXPECT_EQ(gpu.gate->signaledValue(), 0u) << "a write waited for a frame that does not read its texture";
    EXPECT_EQ(gpu.context.renderingStats().numTextureUploadWaits, stats.numTextureUploadWaits);
    EXPECT_EQ(gpu.context.renderingStats().numTexturesReplacedInUse, stats.numTexturesReplacedInUse);

    pass.reset();
    encoder->present(*gpu.target);
    gpu.gate->setSignaledValue(1);
    opener.join();
    EXPECT_EQ(finish(read), filled(0x11));
}

// Buffers are never written in place: an update makes a new MTLBuffer, or for a small one copies
// the bytes into the command buffer when it is bound. So a frame in flight keeps the bytes it was
// encoded with, and the CPU does not wait.
TEST(MetalTexture, ABufferUpdateKeepsTheBytesAFrameInFlightStillReads) {
    GPUFixture gpu;
    const auto before = filled(0x55);
    auto buffer = gpu.context.createBuffer(
        before.data(), texelBytes, gfx::BufferUsageType::StaticDraw, false, /*persistent=*/true);
    ASSERT_TRUE(buffer.getMetalBuffer());

    const auto read = commitGatedRead(gpu.context, *gpu.target, gpu.gate.get(), nullptr, &buffer);
    const auto after = filled(0x66);
    buffer.update(after.data(), texelBytes, 0);
    EXPECT_EQ(gpu.gate->signaledValue(), 0u) << "the update waited for the GPU";
    gpu.gate->setSignaledValue(1);

    EXPECT_EQ(finish(read), before) << "the update overwrote bytes a frame in flight reads";
    EXPECT_EQ(std::memcmp(buffer.contents(), after.data(), texelBytes), 0);
}

#endif
