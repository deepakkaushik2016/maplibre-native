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
#include <mln/mtl/dynamic_texture.hpp>
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

// A texture read by two committed frames, both still on the GPU, waits for both: the newer frame
// may finish first, as nothing orders two frames that only read the texture.
TEST(MetalTexture, ASubRegionWriteWaitsForEveryFrameInFlightThatReadsTheTexture) {
    GPUFixture gpu;
    auto texture = makeTexture(gpu.context, 0x11);
    auto older = NS::TransferPtr(gpu.context.getBackend().getDevice()->newSharedEvent());
    const auto first = commitGatedRead(gpu.context, *gpu.target, older.get(), texture.get());
    const auto second = commitGatedRead(gpu.context, *gpu.target, gpu.gate.get(), texture.get());
    const auto stats = gpu.context.renderingStats();

    // The newer frame is let go first, the older one later.
    auto openNewer = gpu.openGateLater(std::chrono::milliseconds(50));
    auto openOlder = std::thread([older] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        older->setSignaledValue(1);
    });
    const auto patch = filled(0x33);
    texture->uploadSubRegion(patch.data(), {texels, texels}, 0, 0);
    openNewer.join();
    openOlder.join();

    EXPECT_EQ(finish(first), filled(0x11)) << "the write overwrote texels the older frame reads";
    EXPECT_EQ(finish(second), filled(0x11)) << "the write overwrote texels the newer frame reads";
    EXPECT_EQ(gpu.context.renderingStats().numTextureUploadWaits, stats.numTextureUploadWaits + 1);
}

// The tracker never forgets a render pass whose command buffer has not been committed yet: once it
// commits, it is a GPU reader like any other, however many other buffers were tracked meanwhile.
TEST(MetalTexture, APassStillBeingEncodedStaysTrackedAmongManyOthers) {
    GPUFixture gpu;
    auto texture = makeTexture(gpu.context, 0x11);
    const auto& device = gpu.context.getBackend().getDevice();
    auto readback = NS::TransferPtr(device->newBuffer(texelBytes, MTL::ResourceStorageModeShared));

    // The pass that reads the texture, not committed yet.
    auto encoder = gpu.context.createCommandEncoder();
    mtl::MTLCommandBufferPtr reader;
    {
        auto pass = encoder->createRenderPass("pending read", {*gpu.target, Color::white(), {}, {}});
        auto& renderPass = static_cast<mtl::RenderPass&>(*pass);
        texture->bind(renderPass, 0);
        texture->unbind(renderPass, 0);
        reader = renderPass.getCommandBuffer();
    }

    // Many other command buffers, also not committed, from a queue of their own so the backend's
    // queue keeps buffers to hand out.
    auto otherQueue = NS::TransferPtr(device->newCommandQueue(128));
    std::vector<mtl::MTLCommandBufferPtr> others;
    for (int index = 0; index < 100; ++index) {
        others.push_back(NS::RetainPtr(otherQueue->commandBuffer()));
        gpu.context.trackCommandBuffer(others.back());
    }

    reader->encodeWait(gpu.gate.get(), 1);
    auto* blit = reader->blitCommandEncoder();
    blit->copyFromTexture(texture->getMetalTexture(),
                          0,
                          0,
                          MTL::Origin::Make(0, 0, 0),
                          MTL::Size::Make(texels, texels, 1),
                          readback.get(),
                          0,
                          texels * 4,
                          texelBytes);
    blit->endEncoding();
    encoder->present(*gpu.target);

    auto opener = gpu.openGateLater(std::chrono::milliseconds(200));
    const auto patch = filled(0x33);
    texture->uploadSubRegion(patch.data(), {texels, texels}, 0, 0);
    opener.join();
    reader->waitUntilCompleted();
    const auto* bytes = static_cast<const uint8_t*>(readback->contents());
    EXPECT_EQ(std::vector<uint8_t>(bytes, bytes + texelBytes), filled(0x11))
        << "the tracker dropped the reader while it was being encoded";
}

// A small buffer (under 4 KB, not persistent) has no MTLBuffer: it lives in CPU memory and Metal
// copies it into the command buffer when a pass binds it (setVertexBytes), so an update never
// touches memory the GPU reads, and never waits.
TEST(MetalTexture, ASmallBufferIsCopiedIntoTheFrameAndUpdatesWithoutAWait) {
    GPUFixture gpu;
    const auto before = filled(0x55);
    auto buffer = gpu.context.createBuffer(
        before.data(), 64, gfx::BufferUsageType::StaticDraw, false, /*persistent=*/false);
    EXPECT_FALSE(buffer.getMetalBuffer()) << "a small buffer is backed by GPU-visible memory";

    // A held frame binds it; the bytes are in that frame's command buffer now.
    const auto read = commitGatedRead(gpu.context, *gpu.target, gpu.gate.get(), nullptr, &buffer);
    const auto after = filled(0x66);
    buffer.update(after.data(), 64, 0);
    EXPECT_EQ(gpu.gate->signaledValue(), 0u) << "the update waited for the GPU";
    EXPECT_EQ(std::memcmp(buffer.contents(), after.data(), 64), 0);
    gpu.gate->setSignaledValue(1);
    finish(read);
}

// A glyph or icon atlas bin that a layout releases (a tile evicted) must not be handed to the next
// image while a frame that drew the released one is still on the GPU: a worker writes the new
// image into the bin at once, with no wait, and the frame in flight would sample it. The bin goes
// back to the atlas only once every command buffer that may have sampled it has completed.
TEST(MetalTexture, AReleasedAtlasBinIsNotReusedWhileAFrameInFlightSamplesIt) {
    GPUFixture gpu;
    auto atlas = gpu.context.createDynamicTexture({texels, texels}, gfx::TexturePixelType::RGBA);
    const auto first = filled(0x11);
    const auto handle = atlas->addImage(first.data(), {texels, texels});
    ASSERT_TRUE(handle);
    auto* texture = static_cast<mtl::Texture2D*>(atlas->getTexture().get());

    // A frame that draws the image is held on the GPU; then its layout releases the bin, and the
    // next frame, which still draws it, is held too. Both frames end, still on the GPU.
    const auto read = commitGatedRead(gpu.context, *gpu.target, gpu.gate.get(), texture);
    atlas->removeTexture(*handle);
    gpu.context.beginFrame();
    const auto next = commitGatedRead(gpu.context, *gpu.target, gpu.gate.get(), texture);
    gpu.context.beginFrame();

    // Another image of the same size, from a worker. The atlas has room only in that bin.
    const auto second = filled(0x22);
    const auto reused = atlas->addImage(second.data(), {texels, texels});
    EXPECT_FALSE(reused) << "the released bin was handed out while frames in flight sample it";
    EXPECT_EQ(gpu.gate->signaledValue(), 0u) << "the allocation waited for the GPU";
    gpu.gate->setSignaledValue(1);
    EXPECT_EQ(finish(read), first) << "a frame in flight sampled an image written into its bin";
    EXPECT_EQ(finish(next), first) << "the next frame sampled an image written into its bin";
    if (reused) {
        atlas->removeTexture(*reused);
    }

    // Once those frames have completed, the bin is free again, from the next frame on.
    gpu.context.beginFrame();
    const auto later = atlas->addImage(second.data(), {texels, texels});
    ASSERT_TRUE(later) << "the bin never came back once its frames had completed";
    EXPECT_EQ(later->getRectangle(), handle->getRectangle());
    EXPECT_EQ(finish(commitGatedRead(gpu.context, *gpu.target, gpu.gate.get(), texture)), second);
}

// The release stamp counts frames, not render passes: a terrain frame encodes many passes, and a
// drawable that still samples a released bin may be encoded in any of them, in this frame or the
// next. So passes that complete after the release, in the same frame and in the next one, must
// not free the bin; it is free once the frame after the release has ended and completed.
TEST(MetalTexture, AReleasedAtlasBinWaitsForTheNextFrameNotTheNextPass) {
    GPUFixture gpu;
    gpu.gate->setSignaledValue(1); // no pass is held: each completes as soon as it is waited for
    auto atlas = gpu.context.createDynamicTexture({texels, texels}, gfx::TexturePixelType::RGBA);
    auto* texture = static_cast<mtl::Texture2D*>(atlas->getTexture().get());
    const auto first = filled(0x11);
    const auto second = filled(0x22);

    gpu.context.beginFrame(); // the frame that releases the bin
    const auto handle = atlas->addImage(first.data(), {texels, texels});
    ASSERT_TRUE(handle);
    finish(commitGatedRead(gpu.context, *gpu.target, gpu.gate.get(), texture));
    atlas->removeTexture(*handle);
    for (int pass = 0; pass < 3; ++pass) {
        finish(commitGatedRead(gpu.context, *gpu.target, gpu.gate.get(), texture));
    }
    EXPECT_FALSE(atlas->addImage(second.data(), {texels, texels}))
        << "passes completed later in the frame freed the bin";

    gpu.context.beginFrame(); // the next frame may still draw it, in any of its passes
    for (int pass = 0; pass < 3; ++pass) {
        finish(commitGatedRead(gpu.context, *gpu.target, gpu.gate.get(), texture));
    }
    EXPECT_FALSE(atlas->addImage(second.data(), {texels, texels}))
        << "the bin was freed while the frame after the release was still being encoded";

    gpu.context.beginFrame(); // that frame has ended, and its passes have completed
    const auto later = atlas->addImage(second.data(), {texels, texels});
    ASSERT_TRUE(later) << "the bin never came back";
    EXPECT_EQ(later->getRectangle(), handle->getRectangle());
}

#endif
