#include <mln/mtl/dynamic_texture.hpp>
#include <mln/mtl/texture2d.hpp>
#include <mln/mtl/context.hpp>

namespace mln {
namespace mtl {

DynamicTexture::DynamicTexture(Context& context, Size size, gfx::TexturePixelType pixelType)
    : gfx::DynamicTexture(context, size, pixelType) {}

void DynamicTexture::uploadImage(const uint8_t* pixelData, gfx::TextureHandle& texHandle) {
    std::scoped_lock lock(mutex);
    const auto& rect = texHandle.getRectangle();
    const auto imageSize = Size(rect.w, rect.h);

    texture->create();
    // The region was just reserved, so no frame samples it, and this may run on a worker thread:
    // write it with no wait for the GPU.
    static_cast<Texture2D&>(*texture).writeRegion(pixelData, imageSize, rect.x, rect.y);

    gfx::DynamicTexture::uploadImage(pixelData, texHandle);
}

} // namespace mtl
} // namespace mln
