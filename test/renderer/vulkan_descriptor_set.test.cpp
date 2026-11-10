#if MLN_RENDER_BACKEND_VULKAN
#include <mln/test/util.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/dynamic_texture.hpp>
#include <mln/gfx/offscreen_texture.hpp>
#include <mln/gfx/render_pass.hpp>
#include <mln/shaders/layer_ubo.hpp>
#include <mln/shaders/program_parameters.hpp>
#include <mln/shaders/shader_defines.hpp>
#include <mln/shaders/vulkan/shader_program.hpp>
#include <mln/vulkan/command_encoder.hpp>
#include <mln/vulkan/context.hpp>
#include <mln/vulkan/descriptor_set.hpp>
#include <mln/vulkan/headless_backend.hpp>
#include <mln/vulkan/pipeline.hpp>
#include <mln/vulkan/render_pass.hpp>
#include <mln/vulkan/renderable_resource.hpp>
#include <mln/vulkan/texture2d.hpp>
#include <mln/util/logging.hpp>

#include <array>
#include <atomic>
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
    /// Objects destroyed, in order
    std::vector<VkFramebuffer> framebuffersDestroyed;
    std::vector<VkRenderPass> renderPassesDestroyed;
    std::vector<VkImageView> imageViewsDestroyed;
    PFN_vkDestroyFramebuffer destroyFramebuffer = nullptr;
    PFN_vkDestroyRenderPass destroyRenderPass = nullptr;
    PFN_vkDestroyImageView destroyImageView = nullptr;
    PFN_vkCreateGraphicsPipelines createGraphicsPipelines = nullptr;
    /// Graphics pipelines created
    unsigned pipelinesCreated = 0;
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

VKAPI_ATTR VkResult VKAPI_CALL createGraphicsPipelines(VkDevice device,
                                                       VkPipelineCache cache,
                                                       uint32_t count,
                                                       const VkGraphicsPipelineCreateInfo* infos,
                                                       const VkAllocationCallbacks* allocator,
                                                       VkPipeline* pipelines) {
    probe->pipelinesCreated += count;
    return probe->createGraphicsPipelines(device, cache, count, infos, allocator, pipelines);
}

VKAPI_ATTR void VKAPI_CALL destroyFramebuffer(VkDevice device,
                                              VkFramebuffer framebuffer,
                                              const VkAllocationCallbacks* allocator) {
    probe->framebuffersDestroyed.push_back(framebuffer);
    probe->destroyFramebuffer(device, framebuffer, allocator);
}

VKAPI_ATTR void VKAPI_CALL destroyRenderPass(VkDevice device,
                                             VkRenderPass renderPass,
                                             const VkAllocationCallbacks* allocator) {
    probe->renderPassesDestroyed.push_back(renderPass);
    probe->destroyRenderPass(device, renderPass, allocator);
}

VKAPI_ATTR void VKAPI_CALL destroyImageView(VkDevice device,
                                            VkImageView imageView,
                                            const VkAllocationCallbacks* allocator) {
    probe->imageViewsDestroyed.push_back(imageView);
    probe->destroyImageView(device, imageView, allocator);
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
        state.createGraphicsPipelines = dispatcher.vkCreateGraphicsPipelines;
        state.destroyFramebuffer = dispatcher.vkDestroyFramebuffer;
        state.destroyRenderPass = dispatcher.vkDestroyRenderPass;
        state.destroyImageView = dispatcher.vkDestroyImageView;
        probe = &state;
        dispatcher.vkBeginCommandBuffer = beginCommandBuffer;
        dispatcher.vkCmdBindDescriptorSets = cmdBindDescriptorSets;
        dispatcher.vkUpdateDescriptorSets = updateDescriptorSets;
        dispatcher.vkCreateGraphicsPipelines = createGraphicsPipelines;
        dispatcher.vkDestroyFramebuffer = destroyFramebuffer;
        dispatcher.vkDestroyRenderPass = destroyRenderPass;
        dispatcher.vkDestroyImageView = destroyImageView;
    }

    ~DescriptorTest() {
        // Objects the context still holds are destroyed with it, through the probe's entry points
        backend.getDevice()->waitIdle(backend.getDispatcher());
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
// An offscreen target (a drape, hillshade or heatmap target) dropped while a frame that drew into it
// may still run: its framebuffer, render pass and depth image must outlive that frame, as its colour
// image does. They go through the context's deletion queue, which runs once the frame slot that
// dropped them comes round again, after its fence.
TEST(VulkanOffscreenTexture, ATargetDroppedWhileItsFrameIsInFlightOutlivesThatFrame) {
    DescriptorTest<TwoSlotBackend> test;
    auto& context = test.context;
    auto target = context.createOffscreenTexture(
        {64, 64}, gfx::TextureChannelDataType::UnsignedByte, /*depth=*/true, /*stencil=*/true);

    const auto drawInto = [&](gfx::OffscreenTexture& offscreen) {
        auto encoder = context.createCommandEncoder();
        auto pass = encoder->createRenderPass(
            "target", {.renderable = offscreen, .clearColor = Color::white(), .clearDepth = 1.0f, .clearStencil = 0});
    };

    // Frame 1 draws into the target and is submitted, not waited for.
    context.beginFrame();
    drawInto(*target);
    context.submitFrame();
    const auto& resource = target->getResource<vulkan::RenderableResource>();
    const VkFramebuffer framebuffer = resource.getFramebuffer().get();
    const VkRenderPass renderPass = resource.getRenderPass().get();
    ASSERT_NE(framebuffer, VK_NULL_HANDLE);
    ASSERT_NE(renderPass, VK_NULL_HANDLE);

    // Frame 2 takes the other slot while frame 1 may still run, and drops the target.
    context.beginFrame();
    const auto viewsBefore = test.state.imageViewsDestroyed.size();
    target.reset();
    EXPECT_TRUE(test.state.framebuffersDestroyed.empty()) << "the framebuffer went while its frame may run";
    EXPECT_TRUE(test.state.renderPassesDestroyed.empty()) << "the render pass went while its frame may run";
    EXPECT_EQ(test.state.imageViewsDestroyed.size(), viewsBefore) << "an attachment went while its frame may run";
    context.submitFrame();

    // Frames 3 and 4: once frame 2's slot comes round, after its fence, everything has gone.
    for (int frame = 3; frame <= 4; ++frame) {
        context.beginFrame();
        context.submitFrame();
    }
    EXPECT_EQ(test.state.framebuffersDestroyed, std::vector<VkFramebuffer>{framebuffer});
    EXPECT_EQ(test.state.renderPassesDestroyed, std::vector<VkRenderPass>{renderPass});
    EXPECT_GE(test.state.imageViewsDestroyed.size(), viewsBefore + 2) << "the colour and depth views";
}
// Pipelines are cached under a hash that used to include the render pass handle, and each drape
// target makes its own render pass. As the view moved, every new target's first frame built a new
// set of pipelines that stayed cached for the life of the map: on a phone the driver's native
// memory grew by 2 to 3 MB a minute. A pipeline can draw in any render pass compatible with the
// one it was built for, so compatible targets share them.
TEST(VulkanOffscreenTexture, CompatibleTargetsShareTheirPipelines) {
    DescriptorTest<> test;
    auto& context = test.context;
    const auto makeTarget = [&](bool stencil) {
        auto target = context.createOffscreenTexture(
            {64, 64}, gfx::TextureChannelDataType::UnsignedByte, /*depth=*/true, stencil);
        context.beginFrame();
        {
            auto encoder = context.createCommandEncoder();
            auto pass = encoder->createRenderPass(
                "target", {.renderable = *target, .clearColor = Color::white(), .clearDepth = 1.0f, .clearStencil = 0});
        }
        context.submitFrame();
        return target;
    };
    const auto hashOf = [](const gfx::OffscreenTexture& target) {
        vulkan::PipelineInfo info;
        info.setRenderable(target);
        return info.hash();
    };

    const auto first = makeTarget(/*stencil=*/true);
    const auto second = makeTarget(/*stencil=*/true);
    const auto noStencil = makeTarget(/*stencil=*/false);

    EXPECT_EQ(hashOf(*first), hashOf(*second)) << "two targets with the same formats share a pipeline";
    EXPECT_NE(hashOf(*first), hashOf(*noStencil)) << "a target without a stencil has its own pipeline";
    test.backend.getDevice()->waitIdle(test.backend.getDispatcher());
}

namespace {

// Draws a full-target green triangle with one shader, through the shader's pipeline cache.
class TriangleDrawer {
public:
    explicit TriangleDrawer(DescriptorTest<>& test_)
        : test(test_),
          program(test.context.createProgram(shaders::BuiltIn::BackgroundShader,
                                             "triangle",
                                             R"(
void main() {
    const vec2 corners[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
}
)",
                                             R"(
layout(location = 0) out vec4 fragColor;
void main() { fragColor = vec4(0.0, 1.0, 0.0, 1.0); }
)",
                                             ProgramParameters(1.0f, false),
                                             {})) {}

    ~TriangleDrawer() { test.backend.getDevice()->waitIdle(test.backend.getDispatcher()); }

    void draw(gfx::OffscreenTexture& target) {
        auto& context = test.context;
        context.beginFrame();
        {
            auto encoder = context.createCommandEncoder();
            auto pass = encoder->createRenderPass(
                "target", {.renderable = target, .clearColor = Color::white(), .clearDepth = 1.0f, .clearStencil = 0});
            const auto& buffer = static_cast<vulkan::CommandEncoder&>(*encoder).getCommandBuffer();
            const auto& dispatcher = test.backend.getDispatcher();

            vulkan::PipelineInfo info;
            info.setRenderable(target);
            info.setScissorRect({0, 0, target.getSize().width, target.getSize().height});
            info.setDynamicValues(test.backend, buffer);
            buffer->bindPipeline(vk::PipelineBindPoint::eGraphics, program->getPipeline(info).get(), dispatcher);
            buffer->draw(3, 1, 0, 0, dispatcher);
        }
        context.submitFrame();
        test.backend.getDevice()->waitIdle(test.backend.getDispatcher());
    }

private:
    DescriptorTest<>& test;
    vulkan::UniqueShaderProgram program;
};

} // namespace

// The pipeline cache across the life of real drape targets: compatible targets draw through one
// pipeline, and it still draws after the target it was built for has gone, as a pipeline only
// needs a render pass compatible with the one it was created with.
TEST(VulkanOffscreenTexture, APipelineOutlivesTheTargetItWasBuiltFor) {
    DescriptorTest<> test;
    auto& context = test.context;
    TriangleDrawer drawer(test);
    const auto makeTarget = [&](Size size, gfx::TextureChannelDataType type) {
        return context.createOffscreenTexture(size, type, /*depth=*/true, /*stencil=*/true);
    };
    const auto expectGreen = [](gfx::OffscreenTexture& target) {
        const auto image = target.readStillImage();
        ASSERT_TRUE(image.valid());
        const auto* pixel = image.data.get() + (image.size.height / 2 * image.stride()) + image.size.width / 2 * 4;
        EXPECT_EQ(pixel[0], 0);
        EXPECT_EQ(pixel[1], 255);
        EXPECT_EQ(pixel[2], 0);
        EXPECT_EQ(pixel[3], 255);
    };

    auto first = makeTarget({64, 64}, gfx::TextureChannelDataType::UnsignedByte);
    drawer.draw(*first);
    EXPECT_EQ(test.state.pipelinesCreated, 1u);
    expectGreen(*first);

    auto second = makeTarget({64, 64}, gfx::TextureChannelDataType::UnsignedByte);
    drawer.draw(*second);
    EXPECT_EQ(test.state.pipelinesCreated, 1u) << "a compatible target uses the same pipeline";
    expectGreen(*second);

    // The first target goes, with the render pass the pipeline was created for.
    const VkRenderPass firstRenderPass = first->getResource<vulkan::RenderableResource>().getRenderPass().get();
    first.reset();
    for (int frame = 0; frame < 2; ++frame) {
        context.beginFrame();
        context.submitFrame();
    }
    test.backend.getDevice()->waitIdle(test.backend.getDispatcher());
    ASSERT_EQ(test.state.renderPassesDestroyed, std::vector<VkRenderPass>{firstRenderPass});

    // A smaller target too: its viewport and scissor are dynamic state.
    auto third = makeTarget({32, 32}, gfx::TextureChannelDataType::UnsignedByte);
    drawer.draw(*third);
    EXPECT_EQ(test.state.pipelinesCreated, 1u) << "the pipeline outlives the render pass it was built against";
    expectGreen(*third);

    // A different colour format is a different, incompatible render pass.
    auto halfFloat = makeTarget({64, 64}, gfx::TextureChannelDataType::HalfFloat);
    drawer.draw(*halfFloat);
    EXPECT_EQ(test.state.pipelinesCreated, 2u) << "a half-float target gets its own pipeline";
    drawer.draw(*third);
    EXPECT_EQ(test.state.pipelinesCreated, 2u) << "the unsigned byte pipeline stays cached";
}

// A render target's colour image holds megabytes (a terrain drape target is 1024 x 1024 RGBA), so it
// gets its own device memory, like its depth image. Suballocated from the allocator's blocks, it
// would keep a block of 256 MB alive after the target went, because the allocator frees a block
// only once the last allocation in it is gone: on a phone, 750 MB stayed behind when the terrain
// turned off, with 40 MB of it still used. These targets come and go with the view.
TEST(VulkanOffscreenTexture, ATargetsColourImageHasItsOwnDeviceMemory) {
    DescriptorTest<> test;
    auto& context = test.context;
    const auto blocks = [&] {
        VmaTotalStatistics stats;
        vmaCalculateStatistics(context.getBackend().getAllocator(), &stats);
        return stats.total.statistics.blockCount;
    };
    const auto makeTarget = [&] {
        auto target = context.createOffscreenTexture(
            {1024, 1024}, gfx::TextureChannelDataType::UnsignedByte, /*depth=*/true, /*stencil=*/true);
        context.beginFrame();
        {
            auto encoder = context.createCommandEncoder();
            auto pass = encoder->createRenderPass(
                "target", {.renderable = *target, .clearColor = Color::white(), .clearDepth = 1.0f, .clearStencil = 0});
        }
        context.submitFrame();
        return target;
    };

    // The first target: any block the allocator was going to make exists now.
    auto first = makeTarget();
    const auto blocksWithOneTarget = blocks();

    auto second = makeTarget();
    EXPECT_EQ(blocks(), blocksWithOneTarget + 2)
        << "the second target's colour and depth images each take their own memory";

    second.reset();
    first.reset();
    // The deletion queue of a frame slot runs once the slot comes round again.
    for (int frame = 0; frame < 4; ++frame) {
        context.beginFrame();
        context.submitFrame();
    }
    test.backend.getDevice()->waitIdle(test.backend.getDispatcher());
    EXPECT_EQ(blocks(), blocksWithOneTarget - 2) << "the first target's memory went with it";
}

// The dummy fallback hides a texture bound before its image exists, so it says so at debug level,
// at most once per interval, with the count of fallbacks since the last line.
TEST(VulkanDescriptorSet, TheDummyTextureFallbackIsLoggedAtMostOncePerInterval) {
    using namespace std::chrono_literals;
    vulkan::FallbackLogLimiter limiter(10s);
    const auto start = std::chrono::steady_clock::time_point{} + 1h;
    EXPECT_EQ(limiter.count(start), 1u) << "the first fallback is not logged";
    EXPECT_EQ(limiter.count(start + 1s), 0u);
    EXPECT_EQ(limiter.count(start + 9s), 0u);
    EXPECT_EQ(limiter.count(start + 10s), 3u) << "the next line does not count the fallbacks it covers";
    EXPECT_EQ(limiter.count(start + 11s), 0u);
    EXPECT_EQ(limiter.count(start + 25s), 2u);
}

#ifndef NDEBUG
TEST(VulkanDescriptorSet, TheDummyTextureFallbackLogsADebugLine) {
    // Debug lines reach no Log::Observer (Log::record skips the observer for them), only stderr.
    Log::useLogThread(false);
    testing::internal::CaptureStderr();
    DescriptorTest<> test;
    auto& context = test.context;
    auto texture = context.createTexture2D();
    texture->setSize({64, 64});
    texture->setFormat(gfx::TexturePixelType::RGBA, gfx::TextureChannelDataType::UnsignedByte);
    static_cast<vulkan::Texture2D&>(*texture).setUsage(vulkan::Texture2DUsage::Attachment);
    vulkan::ImageDescriptorSet images(context);
    std::array<gfx::Texture2DPtr, shaders::maxTextureCountPerShader> textures{};
    textures[0] = texture;

    test.renderFrames(1, [&](gfx::RenderPass& pass) {
        images.update(textures);
        images.bind(static_cast<vulkan::RenderPass&>(pass).getEncoder());
    });

    const auto output = testing::internal::GetCapturedStderr();
    Log::useLogThread(true);
    Log::useLogThread(false, EventSeverity::Error);
    size_t lines = 0;
    for (size_t at = output.find("[DEBUG]"); at != std::string::npos; at = output.find("[DEBUG]", at + 1)) {
        const auto end = output.find('\n', at);
        if (output.substr(at, end - at).find("bound the dummy texture") != std::string::npos) {
            ++lines;
        }
    }
    EXPECT_EQ(lines, 1u) << "the fallback was not logged once";
}
#endif
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
