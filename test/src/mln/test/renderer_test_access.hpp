#pragma once

#include <mln/gfx/texture2d.hpp>
#include <mln/renderer/layer_group.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/renderer/renderer_impl.hpp>
#include <mln/util/size.hpp>

#include <string>
#include <vector>

namespace mln {

// Renderer and Renderer::Impl befriend this, so tests can look at renderer internals without
// public API for them. One definition for every test: the class is befriended by name, so two
// tests defining it differently would break the one-definition rule.
class RendererTestAccess {
public:
    // How many layer groups named `name` the orchestrator draws.
    static size_t layerGroupCount(Renderer& renderer, const std::string& name) {
        size_t count = 0;
        renderer.impl->orchestrator.visitLayerGroups(
            [&](LayerGroupBase& group) { count += group.getName() == name ? 1 : 0; });
        return count;
    }

    // The texture size of every terrain drape target in the pool.
    static std::vector<Size> drapeTargetSizes(Renderer& renderer) {
        std::vector<Size> sizes;
        renderer.impl->texturePool.visitRenderTargets([&](const std::shared_ptr<RenderTarget>& target) {
            if (const auto& texture = target->getTexture()) {
                sizes.push_back(texture->getSize());
            }
        });
        return sizes;
    }
};

} // namespace mln
