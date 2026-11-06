#pragma once

#include <mln/gfx/offscreen_texture.hpp>
#include <mln/gfx/types.hpp>

namespace mln {
namespace mtl {

class Context;
class Texture2D;

class OffscreenTexture final : public gfx::OffscreenTexture {
public:
    OffscreenTexture(Context&, Size size, gfx::TextureChannelDataType type, bool depth, bool stencil);

    bool isRenderable() override;

    PremultipliedImage readStillImage() override;
    const gfx::Texture2DPtr& getTexture() override;

    /// Waits until the GPU has finished the last pass that drew the texture. Passes are committed,
    /// not waited for, so a reader outside the GPU queue (the CPU, another API) calls this first.
    void waitUntilCompleted();
};

} // namespace mtl
} // namespace mln
