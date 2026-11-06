#if MLN_RENDER_BACKEND_VULKAN
#include <mln/test/util.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/dynamic_texture.hpp>
#include <mln/gfx/render_pass.hpp>
#include <mln/shaders/layer_ubo.hpp>
#include <mln/shaders/shader_defines.hpp>
#include <mln/vulkan/command_encoder.hpp>
#include <mln/vulkan/context.hpp>
#include <mln/vulkan/descriptor_set.hpp>
#include <mln/vulkan/headless_backend.hpp>
#include <mln/vulkan/render_pass.hpp>
#include <mln/vulkan/renderable_resource.hpp>
#include <mln/vulkan/texture2d.hpp>

#include <array>
#include <map>
#include <set>
#include <vector>

using namespace mln;

// A descriptor set bound into a command buffer must not be written while that buffer records or
// runs: the buffer is invalidated, and what the passes bound before the write read is undefined
// (VUID-vkCmdBindDescriptorSets-commandBuffer-recording, "destroyed or updated without
// UPDATE_AFTER_BIND"). The terrain drape targets did just that: each swaps the global paint params
// for its own, between render passes of one frame's command buffer, which wrote the one global set
// the earlier passes had bound. These tests stand in for the validation layer: they watch the
// device calls through the backend's dispatcher.

namespace {

struct DescriptorProbe {
    /// The sets bound into each command buffer since it began recording.
    std::map<VkCommandBuffer, std::set<VkDescriptorSet>> bound;
    unsigned binds = 0;
    /// Writes to a set bound into a command buffer that has not begun recording again since.
    unsigned rewrites = 0;
    /// The image views written into image descriptors.
    std::vector<VkImageView> imageViews;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkCmdBindDescriptorSets cmdBindDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets updateDescriptorSets = nullptr;
};

DescriptorProbe* probe = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL beginCommandBuffer(VkCommandBuffer buffer, const VkCommandBufferBeginInfo* info) {
    probe->bound.erase(buffer);
    return probe->beginCommandBuffer(buffer, info);
}

VKAPI_ATTR void VKAPI_CALL cmdBindDescriptorSets(VkCommandBuffer buffer,
                                                 VkPipelineBindPoint bindPoint,
                                                 VkPipelineLayout layout,
                                                 uint32_t firstSet,
                                                 uint32_t count,
                                                 const VkDescriptorSet* sets,
                                                 uint32_t dynamicOffsetCount,
                                                 const uint32_t* dynamicOffsets) {
    for (uint32_t i = 0; i < count; ++i) {
        probe->bound[buffer].insert(sets[i]);
        ++probe->binds;
    }
    probe->cmdBindDescriptorSets(buffer, bindPoint, layout, firstSet, count, sets, dynamicOffsetCount, dynamicOffsets);
}

VKAPI_ATTR void VKAPI_CALL updateDescriptorSets(VkDevice device,
                                                uint32_t writeCount,
                                                const VkWriteDescriptorSet* writes,
                                                uint32_t copyCount,
                                                const VkCopyDescriptorSet* copies) {
    for (uint32_t i = 0; i < writeCount; ++i) {
        if (writes[i].pImageInfo) {
            probe->imageViews.push_back(writes[i].pImageInfo->imageView);
        }
        for (const auto& [buffer, sets] : probe->bound) {
            if (sets.contains(writes[i].dstSet)) {
                ++probe->rewrites;
            }
        }
    }
    probe->updateDescriptorSets(device, writeCount, writes, copyCount, copies);
}

// A renderable stood in for a surface, without one: its images are the frame slots' own.
class SlotResource final : public vulkan::SurfaceRenderableResource {
public:
    explicit SlotResource(vulkan::RendererBackend& backend_)
        : SurfaceRenderableResource(backend_) {}
    void createPlatformSurface() override {}
    void bind() override {}
};

// The headless backend keeps one frame slot (RendererBackend::initSwapchain); a surface keeps two,
// so the frame before is still in flight while the next records. This backend keeps two without a
// surface, as the FFI's texture backend picks its own count.
class TwoSlotBackend final : public vulkan::RendererBackend, public gfx::Renderable {
public:
    explicit TwoSlotBackend(Size size_)
        : vulkan::RendererBackend(gfx::ContextMode::Unique),
          gfx::Renderable(size_, nullptr) {
        // initSurface asks the renderable for its surface, so it exists before init
        setResource(std::make_unique<SlotResource>(*this));
        init();
    }
    ~TwoSlotBackend() override {
        gfx::BackendScope guard{*this, gfx::BackendScope::ScopeType::Implicit};
        resource.reset();
        getThreadPool().runRenderJobs(true /* closeQueue */);
        context.reset();
    }
    gfx::Renderable& getDefaultRenderable() override { return *this; }

protected:
    void activate() override {}
    void deactivate() override {}
    void initSwapchain() override {
        maxFrames = 2;
        getResource<SlotResource>().init(getSize().width, getSize().height);
    }
};

// A backend whose descriptor set calls go through the probe.
template <typename Backend = vulkan::HeadlessBackend>
class DescriptorTest {
public:
    DescriptorTest()
        : backend(Size{64, 64}),
          scope(backend),
          context(backend.template getContext<vulkan::Context>()),
          dispatcher(const_cast<vulkan::DispatchLoaderDynamic&>(backend.getDispatcher())),
          original(dispatcher) {
        // The backend exposes its dispatcher read-only; swapping entry points is the only way
        // to watch the device calls.
        state.beginCommandBuffer = dispatcher.vkBeginCommandBuffer;
        state.cmdBindDescriptorSets = dispatcher.vkCmdBindDescriptorSets;
        state.updateDescriptorSets = dispatcher.vkUpdateDescriptorSets;
        probe = &state;
        dispatcher.vkBeginCommandBuffer = beginCommandBuffer;
        dispatcher.vkCmdBindDescriptorSets = cmdBindDescriptorSets;
        dispatcher.vkUpdateDescriptorSets = updateDescriptorSets;
    }

    ~DescriptorTest() {
        dispatcher = original;
        probe = nullptr;
    }

    std::unique_ptr<gfx::RenderPass> createPass(gfx::CommandEncoder& encoder) {
        return encoder.createRenderPass("probe",
                                        {.renderable = backend.getDefaultRenderable(),
                                         .clearColor = Color::white(),
                                         .clearDepth = 1.0f,
                                         .clearStencil = 0});
    }

    // Runs `record` in a render pass on the default renderable, in each of `frames` frames.
    template <typename F>
    void renderFrames(int frames, F record) {
        renderFramesWithEncoder(frames, [&](gfx::CommandEncoder& encoder) {
            auto pass = createPass(encoder);
            record(*pass);
        });
    }

    // Runs `record` with the frame's encoder in each of `frames` frames. Each frame is submitted
    // without waiting for it: beginFrame waits only for the last frame of the slot it takes, so
    // with two slots the frame before is still in flight.
    template <typename F>
    void renderFramesWithEncoder(int frames, F record) {
        for (int frame = 0; frame < frames; ++frame) {
            context.beginFrame();
            {
                auto encoder = context.createCommandEncoder();
                record(*encoder);
            }
            context.submitFrame();
        }
        backend.getDevice()->waitIdle(backend.getDispatcher());
    }

    Backend backend;
    gfx::BackendScope scope;
    vulkan::Context& context;
    vulkan::DispatchLoaderDynamic& dispatcher;
    const vulkan::DispatchLoaderDynamic original;
    DescriptorProbe state;
};

} // namespace

// The global uniforms, as a terrain frame binds them: the main pass's paint params, a drape
// target's own, then the main pass's again, all in one frame's command buffer.
TEST(VulkanDescriptorSet, TheGlobalSetIsNotWrittenAgainInTheFrameThatBoundIt) {
    DescriptorTest<> test;
    auto& context = test.context;
    const shaders::GlobalPaintParamsUBO params{};
    const auto mainParams = context.createUniformBuffer(&params, sizeof(params), false);
    const auto drapeParams = context.createUniformBuffer(&params, sizeof(params), false);
    auto& globals = context.mutableGlobalUniformBuffers();

    test.renderFrames(4, [&](gfx::RenderPass& pass) {
        for (const auto& ubo : {mainParams, drapeParams, mainParams}) {
            globals.set(shaders::idGlobalPaintParamsUBO, ubo);
            context.bindGlobalUniformBuffers(pass);
        }
    });
    globals.set(shaders::idGlobalPaintParamsUBO, nullptr);

    EXPECT_EQ(test.state.binds, 12u);
    EXPECT_EQ(test.state.rewrites, 0u) << "a global descriptor set bound in the frame was written again in it";
}

// A drawable's image set whose textures change after it was drawn earlier in the frame, as a draped
// drawable drawn into one drape target and then another.
TEST(VulkanDescriptorSet, AnImageSetIsNotWrittenAgainInTheFrameThatBoundIt) {
    DescriptorTest<> test;
    vulkan::ImageDescriptorSet images(test.context);
    const std::array<gfx::Texture2DPtr, shaders::maxTextureCountPerShader> textures{};

    test.renderFrames(4, [&](gfx::RenderPass& pass) {
        auto& encoder = static_cast<vulkan::RenderPass&>(pass).getEncoder();
        for (int draw = 0; draw < 3; ++draw) {
            images.markDirty();
            images.update(textures);
            images.bind(encoder);
        }
    });

    EXPECT_EQ(test.state.binds, 12u);
    EXPECT_EQ(test.state.rewrites, 0u) << "an image descriptor set bound in the frame was written again in it";
}
// The same with two frame slots and the frame structure of a terrain frame: the main pass binds the
// main paint params, each drape target's pass its own, and the main passes after them the main
// params again, every frame while the frame before may still run. A set retired in one frame must
// stay out of use until that frame has completed, and the other slot's sets, which the frame in
// flight may read, must not be written.
TEST(VulkanDescriptorSet, NoSetInUseIsWrittenWithTwoFramesInFlight) {
    DescriptorTest<TwoSlotBackend> test;
    auto& context = test.context;
    ASSERT_EQ(test.backend.getMaxFrames(), 2u);
    const shaders::GlobalPaintParamsUBO params{};
    const auto mainParams = context.createUniformBuffer(&params, sizeof(params), false);
    const auto drapeParams = context.createUniformBuffer(&params, sizeof(params), false);
    auto& globals = context.mutableGlobalUniformBuffers();
    vulkan::ImageDescriptorSet images(context);
    const std::array<gfx::Texture2DPtr, shaders::maxTextureCountPerShader> textures{};

    unsigned passes = 0;
    test.renderFramesWithEncoder(8, [&](gfx::CommandEncoder& encoder) {
        for (const auto& ubo : {mainParams, drapeParams, drapeParams, mainParams}) {
            auto pass = test.createPass(encoder);
            globals.set(shaders::idGlobalPaintParamsUBO, ubo);
            context.bindGlobalUniformBuffers(*pass);
            // A draped drawable drawn into each target, its textures changed in between
            images.markDirty();
            images.update(textures);
            images.bind(static_cast<vulkan::RenderPass&>(*pass).getEncoder());
            ++passes;
        }
    });
    globals.set(shaders::idGlobalPaintParamsUBO, nullptr);

    EXPECT_EQ(passes, 32u);
    EXPECT_EQ(test.state.binds, 64u);
    EXPECT_EQ(test.state.rewrites, 0u) << "a descriptor set in use was written with two frames in flight";
}

// A texture bound before anything created its image, as the terrain depth target was before its
// pass first ran: the drawable's image set must not dereference the missing image view. It binds
// the context's dummy texture in its place.
TEST(VulkanDescriptorSet, AnUncreatedTextureBindsTheDummyTexture) {
    DescriptorTest<> test;
    auto& context = test.context;
    auto texture = context.createTexture2D();
    auto& textureImpl = static_cast<vulkan::Texture2D&>(*texture);
    texture->setSize({64, 64});
    texture->setFormat(gfx::TexturePixelType::RGBA, gfx::TextureChannelDataType::UnsignedByte);
    textureImpl.setUsage(vulkan::Texture2DUsage::Attachment);
    ASSERT_FALSE(textureImpl.hasImage());
    vulkan::ImageDescriptorSet images(context);
    std::array<gfx::Texture2DPtr, shaders::maxTextureCountPerShader> textures{};
    textures[0] = texture;

    test.renderFrames(1, [&](gfx::RenderPass& pass) {
        images.update(textures);
        images.bind(static_cast<vulkan::RenderPass&>(pass).getEncoder());
    });

    ASSERT_FALSE(test.state.imageViews.empty());
    const VkImageView dummy = context.getDummyTexture()->getVulkanImageView().get();
    EXPECT_EQ(test.state.imageViews.front(), dummy) << "the uncreated texture's slot is not the dummy texture";
}

// An atlas bin released while frames are in flight comes back once the frame slot fences show that
// every frame that may have drawn it has completed: with two slots, the release in frame 2 (stamped
// 3, as frame 3 may still draw it) is free from frame 5, whose slot's fence covers frame 3.
TEST(VulkanDynamicTexture, AReleasedBinComesBackOnceTheFrameFencesCoverItsFrames) {
    DescriptorTest<TwoSlotBackend> test;
    auto& context = test.context;
    auto atlas = context.createDynamicTexture({16, 16}, gfx::TexturePixelType::Alpha);
    std::vector<uint8_t> pixels(16 * 16, 0xff);

    context.beginFrame(); // frame 1
    const auto drawn = atlas->addImage(pixels.data(), {16, 16});
    ASSERT_TRUE(drawn);
    context.submitFrame();

    context.beginFrame(); // frame 2
    atlas->removeTexture(*drawn);
    int frame = 2;
    std::optional<gfx::TextureHandle> reused;
    while (!(reused = atlas->reserveSize({16, 16}, -1)) && frame < 8) {
        context.submitFrame();
        context.beginFrame();
        ++frame;
    }
    context.submitFrame();
    ASSERT_TRUE(reused) << "the bin never came back";
    EXPECT_EQ(frame, 5) << "the bin came back in the wrong frame";
    EXPECT_EQ(reused->getRectangle(), drawn->getRectangle());
}
#endif
