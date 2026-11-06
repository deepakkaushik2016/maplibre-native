#pragma once

#include <mln/gfx/draw_mode.hpp>
#include <mln/gfx/depth_mode.hpp>
#include <mln/gfx/stencil_mode.hpp>
#include <mln/gfx/color_mode.hpp>
#include <mln/gfx/texture2d.hpp>
#include <mln/gfx/context.hpp>
#include <mln/mtl/buffer_resource.hpp>
#include <mln/mtl/mtl_fwd.hpp>
#include <mln/mtl/uniform_buffer.hpp>
#include <mln/util/noncopyable.hpp>
#include <mln/util/containers.hpp>

#include <deque>
#include <memory>
#include <optional>
#include <vector>

namespace mln {

class ProgramParameters;
class RenderStaticData;

namespace gfx {
class VertexAttributeArray;
using VertexAttributeArrayPtr = std::shared_ptr<VertexAttributeArray>;
} // namespace gfx

namespace shaders {
struct ClipUBO;
} // namespace shaders

namespace mtl {

class RenderPass;
class RendererBackend;
class ShaderProgram;
class VertexBufferResource;

using UniqueShaderProgram = std::unique_ptr<ShaderProgram>;
using UniqueVertexBufferResource = std::unique_ptr<VertexBufferResource>;
using UniqueUniformBufferArray = std::unique_ptr<gfx::UniformBufferArray>;

class Context final : public gfx::Context {
public:
    Context(RendererBackend&);
    ~Context() noexcept override;
    Context(const Context&) = delete;
    Context& operator=(const Context& other) = delete;

    const RendererBackend& getBackend() const { return backend; }

    void beginFrame() override;
    void endFrame() override;

    std::unique_ptr<gfx::CommandEncoder> createCommandEncoder() override;

    /// Create a new buffer object
    /// @param data The raw data to copy, may be `nullptr`
    /// @param size The size of the buffer
    /// @param usage Not currently used
    /// @param isIndexBuffer True if the buffer will be used for indexes.  The Metal API only accepts `MTLBuffer`
    /// objects for drawing indexed primitives, so this constrains how the buffer can be managed.
    /// @param persistent Performance hint, assume this buffer will be reused many times.
    BufferResource createBuffer(
        const void* data, std::size_t size, gfx::BufferUsageType usage, bool isIndexBuffer, bool persistent) const;

    UniqueShaderProgram createProgram(shaders::BuiltIn shaderID,
                                      std::string name,
                                      std::string_view source,
                                      std::string_view vertexName,
                                      std::string_view fragmentName,
                                      const ProgramParameters& programParameters,
                                      const mln::unordered_map<std::string, std::string>& additionalDefines);

    MTLTexturePtr createMetalTexture(MTLTextureDescriptorPtr textureDescriptor) const;
    MTLSamplerStatePtr createMetalSamplerState(MTLSamplerDescriptorPtr samplerDescriptor) const;

    /// Called at the end of a frame.
    void performCleanup() override;

    void reduceMemoryUsage() override {}

    gfx::UniqueDrawableBuilder createDrawableBuilder(std::string name) override;
    gfx::UniformBufferPtr createUniformBuffer(const void* data,
                                              std::size_t size,
                                              bool persistent = false,
                                              bool ssbo = false) override;

    UniqueUniformBufferArray createLayerUniformBufferArray() override;

    gfx::ShaderProgramBasePtr getGenericShader(gfx::ShaderRegistry&, const std::string& name) override;

    TileLayerGroupPtr createTileLayerGroup(int32_t layerIndex,
                                           std::size_t initialCapacity,
                                           std::string name,
                                           bool renderToTerrain) override;

    LayerGroupPtr createLayerGroup(int32_t layerIndex,
                                   std::size_t initialCapacity,
                                   std::string name,
                                   bool renderToTerrain) override;

    gfx::Texture2DPtr createTexture2D() override;

    /// A 1x1 texture for a sampler slot a shader declares but a drawable leaves empty, such as the
    /// terrain DEM and depth samplers with terrain off (Drawable::bindTextures). Metal's API
    /// validation aborts on a missing binding; Vulkan and WebGPU bind a dummy the same way.
    const gfx::Texture2DPtr& getDummyTexture();

    gfx::DynamicTexturePtr createDynamicTexture(Size size, gfx::TexturePixelType pixelType) override;

    RenderTargetPtr createRenderTarget(const Size size, const gfx::TextureChannelDataType type, bool stencil) override;

    void resetState(gfx::DepthMode depthMode, gfx::ColorMode colorMode) override;

    void setDirtyState() override;

    std::unique_ptr<gfx::OffscreenTexture> createOffscreenTexture(Size,
                                                                  gfx::TextureChannelDataType,
                                                                  bool,
                                                                  bool) override;

    std::unique_ptr<gfx::OffscreenTexture> createOffscreenTexture(Size, gfx::TextureChannelDataType) override;

    std::unique_ptr<gfx::RenderbufferResource> createRenderbufferResource(gfx::RenderbufferPixelType,
                                                                          Size size) override;

    std::unique_ptr<gfx::DrawScopeResource> createDrawScopeResource() override;

    gfx::VertexAttributeArrayPtr createVertexAttributeArray() const override;

#if !defined(NDEBUG)
    void visualizeStencilBuffer() override;
    void visualizeDepthBuffer(float depthRangeSize) override;
#endif

    void clearStencilBuffer(int32_t) override;

    MTLDepthStencilStatePtr makeDepthStencilState(const gfx::DepthMode&,
                                                  const gfx::StencilMode&,
                                                  const gfx::Renderable&) const;

    bool emplaceOrUpdateUniformBuffer(gfx::UniformBufferPtr&,
                                      const void* data,
                                      std::size_t size,
                                      bool persistent) override;

    /// Get a reusable buffer containing the standard fixed tile vertices (+/- `util::EXTENT`)
    const BufferResource& getTileVertexBuffer();

    /// Get a reusable buffer containing the standard fixed tile indexes
    const BufferResource& getTileIndexBuffer();

    /// Get a buffer to be bound to unused vertex buffers
    const UniqueVertexBufferResource& getEmptyVertexBuffer();

    bool renderTileClippingMasks(gfx::RenderPass& renderPass,
                                 RenderStaticData& staticData,
                                 const std::vector<shaders::ClipUBO>& tileUBOs);

    /// Get the global uniform buffers
    const gfx::UniformBufferArray& getGlobalUniformBuffers() const override { return globalUniformBuffers; };

    /// Get the mutable global uniform buffer array
    gfx::UniformBufferArray& mutableGlobalUniformBuffers() override { return globalUniformBuffers; };

    /// Bind the global uniform buffers
    void bindGlobalUniformBuffers(gfx::RenderPass&) const noexcept override;

    /// Unbind the global uniform buffers
    void unbindGlobalUniformBuffers(gfx::RenderPass&) const noexcept override {}

    /// GPU readers of a resource that the CPU writes in place (Texture2D uploads).
    ///
    /// A host need not wait for a frame's GPU work before it encodes the next one, so a texture
    /// that a frame in flight samples may be written again by the CPU, which Metal does not order
    /// against the GPU. Each render pass registers its command buffer here and gets a serial; a
    /// texture bound in the pass keeps that serial. Before a CPU write, the texture asks whether a
    /// committed command buffer up to its serial is still on the GPU. One that has not been
    /// committed yet runs after the write, as the frame being encoded expects.
    ///
    /// Render thread only. Cost: one entry per render pass, dropped once its buffer completes, or
    /// once its renderable drops it uncommitted (untrackCommandBuffer, or when only the tracker
    /// still holds it). A pending entry is never evicted to bound the list.
    using CommandBufferSerial = uint64_t;
    CommandBufferSerial trackCommandBuffer(const MTLCommandBufferPtr&);
    /// Forgets a tracked command buffer that its renderable drops without committing, so the
    /// tracker doesn't keep it (and what it references) alive. A buffer that may still be
    /// committed must not be forgotten.
    void untrackCommandBuffer(const MTL::CommandBuffer*);
    /// Whether the command buffer with `serial` is registered and not yet committed.
    bool isCommandBufferPending(CommandBufferSerial serial) const;
    /// Whether a committed command buffer up to `serial` may still be running.
    bool isCommandBufferInFlight(CommandBufferSerial serial);
    /// Waits for every committed command buffer up to `serial`. Returns whether any was running.
    bool waitForCommandBuffers(CommandBufferSerial serial);

private:
    RendererBackend& backend;
    bool cleanupOnDestruction = true;

    std::optional<BufferResource> emptyBuffer;
    std::optional<BufferResource> tileVertexBuffer;
    std::optional<BufferResource> tileIndexBuffer;

    UniqueVertexBufferResource emptyVertexBuffer;

    gfx::Texture2DPtr dummyTexture;

    gfx::ShaderProgramBasePtr clipMaskShader;
    MTLDepthStencilStatePtr clipMaskDepthStencilState;
    MTLRenderPipelineStatePtr clipMaskPipelineState;
    std::optional<BufferResource> clipMaskUniformsBuffer;
    bool clipMaskUniformsBufferUsed = false;
    const gfx::Renderable* stencilStateRenderable = nullptr;

    UniformBufferArray globalUniformBuffers;

    struct TrackedCommandBuffer {
        CommandBufferSerial serial;
        MTLCommandBufferPtr buffer;
    };
    /// In registration order, and so in serial order
    std::vector<TrackedCommandBuffer> trackedCommandBuffers;
    CommandBufferSerial lastCommandBufferSerial = 0;
    /// Drops finished buffers from the tracker, and publishes the frames that have completed
    void dropFinishedCommandBuffers();

    /// Frames for FrameSerials: the one being encoded, and each ended one with the last command
    /// buffer it tracked, until that buffer and every one before it have completed.
    struct EndedFrame {
        uint64_t frame;
        CommandBufferSerial lastCommandBuffer;
    };
    uint64_t frameSerial = 0;
    std::deque<EndedFrame> endedFrames;
};

} // namespace mtl
} // namespace mln
