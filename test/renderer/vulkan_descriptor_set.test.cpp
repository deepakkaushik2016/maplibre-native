#if MLN_RENDER_BACKEND_VULKAN
#include <mln/test/util.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/render_pass.hpp>
#include <mln/shaders/layer_ubo.hpp>
#include <mln/shaders/shader_defines.hpp>
#include <mln/vulkan/command_encoder.hpp>
#include <mln/vulkan/context.hpp>
#include <mln/vulkan/descriptor_set.hpp>
#include <mln/vulkan/headless_backend.hpp>
#include <mln/vulkan/render_pass.hpp>

#include <array>
#include <map>
#include <set>

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
        for (const auto& [buffer, sets] : probe->bound) {
            if (sets.contains(writes[i].dstSet)) {
                ++probe->rewrites;
            }
        }
    }
    probe->updateDescriptorSets(device, writeCount, writes, copyCount, copies);
}

// A headless backend whose descriptor set calls go through the probe.
class DescriptorTest {
public:
    DescriptorTest()
        : backend({64, 64}),
          scope(backend),
          context(backend.getContext<vulkan::Context>()),
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

    // Runs `record` in a render pass on the default renderable, in each of `frames` frames.
    template <typename F>
    void renderFrames(int frames, F record) {
        for (int frame = 0; frame < frames; ++frame) {
            context.beginFrame();
            {
                auto encoder = context.createCommandEncoder();
                auto pass = encoder->createRenderPass("probe",
                                                      {.renderable = backend.getDefaultRenderable(),
                                                       .clearColor = Color::white(),
                                                       .clearDepth = 1.0f,
                                                       .clearStencil = 0});
                record(*pass);
            }
            context.submitFrame();
            ASSERT_TRUE(context.waitFrame());
        }
    }

    vulkan::HeadlessBackend backend;
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
    DescriptorTest test;
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
    DescriptorTest test;
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
#endif
