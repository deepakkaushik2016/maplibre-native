#pragma once

#include <mln/gfx/dynamic_texture.hpp>

namespace mln {
namespace gl {

class Context;

class DynamicTexture : public gfx::DynamicTexture {
public:
    DynamicTexture(Context& context, Size size, gfx::TexturePixelType pixelType);

    void uploadImage(const uint8_t* pixelData, gfx::TextureHandle& texHandle) override;
    void uploadDeferredImages(gfx::UploadPass&) override;

    using ImagesToUpload =
        std::unordered_map<gfx::TextureHandle, std::unique_ptr<uint8_t[]>, gfx::TextureHandle::Hasher>;

protected:
    void onBinReleased(const gfx::TextureHandle& texHandle) override;

private:
    bool deferredCreation = false;
    ImagesToUpload imagesToUpload;
};

} // namespace gl
} // namespace mln
