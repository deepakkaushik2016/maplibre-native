#pragma once

#include <mln/gfx/texture2d.hpp>
#include <mln/mtl/mtl_fwd.hpp>
#include <mln/util/image.hpp>

#include <Foundation/NSSharedPtr.hpp>
#include <Metal/MTLPixelFormat.hpp>
#include <Metal/MTLTexture.hpp>

#include <memory>

namespace mln {
namespace mtl {

class Context;
class RenderPass;

class Texture2D : public gfx::Texture2D {
public:
    Texture2D(Context& context_);
    ~Texture2D() override;

    gfx::Texture2D& setSamplerConfiguration(const SamplerState&) noexcept override;

    gfx::Texture2D& setFormat(gfx::TexturePixelType, gfx::TextureChannelDataType) noexcept override;

    gfx::Texture2D& setSize(Size size_) noexcept override;

    gfx::Texture2D& setImage(std::shared_ptr<PremultipliedImage>) noexcept override;

    gfx::TexturePixelType getFormat() const noexcept override { return pixelFormat; }

    Size getSize() const noexcept override { return size; }

    size_t getDataSize() const noexcept override;

    size_t getPixelStride() const noexcept override;

    size_t numChannels() const noexcept override;

    void create() override;

    /// A whole-texture upload while a frame in flight on the GPU still samples the texture goes to a
    /// new texture, so the frame keeps what it was encoded with and the CPU does not wait.
    void upload(const void* pixelData, const Size& size_) override;
    /// A sub-region write keeps the rest of the texture, so while a frame in flight samples it, the
    /// write waits for that frame (RenderingStats::numTextureUploadWaits). Render thread only.
    void uploadSubRegion(const void* pixelData, const Size& size, uint16_t xOffset, uint16_t yOffset) noexcept override;
    /// Writes a region with no wait for the GPU, for a caller that knows no frame samples it, such
    /// as a dynamic texture filling a region it has just reserved, from a worker thread.
    void writeRegion(const void* pixelData, const Size& size, uint16_t xOffset, uint16_t yOffset) noexcept;
    void upload() override;

    bool needsUpload() const noexcept override { return !!image; };

    gfx::Texture2D& setUsage(MTL::TextureUsage usage_) noexcept;

    MTL::Texture* getMetalTexture() const noexcept;

    void updateSamplerConfiguration();

    /// @brief Bind this texture to the specified location
    /// @param renderPass Render pass on which the texture will be assign
    /// @param location Location index of texture sampler in a shader
    void bind(RenderPass& renderPass, int32_t location);

    /// @brief Unbind the texture, if it was bound
    /// @param renderPass Render pass from which the texture will be removed
    /// @param location Location index of texture sampler in a shader
    void unbind(RenderPass& renderPass, int32_t location) noexcept;

private:
    MTL::PixelFormat getMetalPixelFormat() const noexcept;
    void createMetalTexture();
    void destroyMetalTexture() noexcept;

    Context& context;
    MTLTexturePtr metalTexture;
    MTLSamplerStatePtr metalSamplerState;

    Size size{0, 0};
    gfx::TexturePixelType pixelFormat{gfx::TexturePixelType::RGBA};
    gfx::TextureChannelDataType channelType{gfx::TextureChannelDataType::UnsignedByte};
    MTL::TextureUsage usage{MTL::TextureUsageShaderRead};
    SamplerState samplerState{};

    std::shared_ptr<PremultipliedImage> image{nullptr};
    /// The command buffer serials of the last two render passes, on different command buffers, that
    /// bound this texture (Context::trackCommandBuffer), or 0. While the last one is still being
    /// encoded, the GPU readers to wait for are up to the one before.
    std::uint64_t lastReadSerial = 0;
    std::uint64_t previousReadSerial = 0;
    /// The serial up to which committed command buffers may still read this texture, or 0
    std::uint64_t committedReadSerial() const;
    bool textureDirty{true};
    bool samplerStateDirty{true};
};

} // namespace mtl
} // namespace mln
