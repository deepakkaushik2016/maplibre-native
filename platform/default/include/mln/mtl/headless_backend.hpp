#pragma once

#include <mln/gfx/headless_backend.hpp>
#include <mln/mtl/mtl_fwd.hpp>
#include <mln/mtl/renderer_backend.hpp>
#include <memory>
#include <functional>

namespace mln {
namespace mtl {

class HeadlessBackend final : public mtl::RendererBackend, public gfx::HeadlessBackend {
public:
    HeadlessBackend(Size = {256, 256},
                    SwapBehaviour = SwapBehaviour::NoFlush,
                    gfx::ContextMode = gfx::ContextMode::Unique);
    ~HeadlessBackend() override;
    gfx::Renderable& getDefaultRenderable() override;
    PremultipliedImage readStillImage() override;
    /// The frame's texture, with the GPU's work on it finished. Blocks until the last pass that drew
    /// it has completed on the GPU, so call it on the thread that renders with this backend, between
    /// frames, never from a thread that the frame's GPU work waits on.
    MTL::Texture* getMetalTexture();
    RendererBackend* getRendererBackend() override;
    SwapBehaviour getSwapBehaviour();

private:
    void activate() override;
    void deactivate() override;
    void ensureResource();

private:
    bool active = false;
    SwapBehaviour swapBehaviour = SwapBehaviour::NoFlush;
};

} // namespace mtl
} // namespace mln
