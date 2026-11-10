#include <mln/renderer/texture_pool.hpp>
#include <mln/gfx/context.hpp>
#include <mln/gfx/offscreen_texture.hpp>
#include <mln/util/logging.hpp>

#include <vector>

namespace mln {
TexturePool::TexturePool(uint32_t tilesize)
    : tileSize(tilesize) {}

TexturePool::~TexturePool() = default;

bool TexturePool::createRenderTarget(gfx::Context& context, const UnwrappedTileID& id, const Color& backgroundColor) {
    // Keep an existing render target; recreating it every frame churns GL
    // texture memory and invalidates the texture bound to the terrain drawable
    if (auto it = renderTargets.find(id); it != renderTargets.end() && it->second) {
        it->second->setClearColor(backgroundColor);
        return false;
    }
    // Drape targets render several overlapping draped tiles (a parent standing in for missing
    // children beside already-loaded children); the stencil clips them against each other,
    // as maplibre-gl-js does for its render-to-texture framebuffer.
    auto renderTarget = context.createRenderTarget(
        {tileSize, tileSize}, gfx::TextureChannelDataType::UnsignedByte, /*stencil=*/true);
    renderTarget->setClearColor(backgroundColor);
    renderTarget->setDrapeTileID(id);
    renderTargets[id] = std::move(renderTarget);
    return true;
}

bool TexturePool::updateRenderTargets(gfx::Context& context,
                                      const std::set<UnwrappedTileID>& cover,
                                      const Color& backgroundColor) {
    // The targets of tiles that left the cover lend their offscreen textures to the targets
    // of tiles that joined it. All drape targets have the same size and formats, and making
    // one's texture and depth-stencil buffer is driver work on the render thread: a fly-to
    // that crosses a zoom level replaces the whole cover (about 60 targets) in one frame,
    // which on a phone stalled that frame for 100-200 ms. A lent texture is cleared and
    // drawn anew, as a new one is, since the new target has baked nothing yet. Only a target
    // the pool alone holds lends its texture; one held elsewhere keeps it. What isn't
    // lent this frame is freed, so no texture is kept back at rest.
    std::vector<std::unique_ptr<gfx::OffscreenTexture>> spare;
    for (auto it = renderTargets.begin(); it != renderTargets.end();) {
        if (cover.contains(it->first)) {
            ++it;
            continue;
        }
        if (it->second && it->second.use_count() == 1) {
            if (auto offscreen = it->second->takeOffscreenTexture()) {
                spare.push_back(std::move(offscreen));
            }
        }
        it = renderTargets.erase(it);
    }

    bool created = false;
    for (const auto& id : cover) {
        if (auto it = renderTargets.find(id); it != renderTargets.end() && it->second) {
            it->second->setClearColor(backgroundColor);
            continue;
        }
        std::shared_ptr<RenderTarget> renderTarget;
        if (!spare.empty()) {
            renderTarget = std::make_shared<RenderTarget>(context, std::move(spare.back()));
            spare.pop_back();
        } else {
            // Drape targets render several overlapping draped tiles; the stencil clips them
            // against each other (see createRenderTarget).
            renderTarget = context.createRenderTarget(
                {tileSize, tileSize}, gfx::TextureChannelDataType::UnsignedByte, /*stencil=*/true);
        }
        renderTarget->setClearColor(backgroundColor);
        renderTarget->setDrapeTileID(id);
        renderTargets[id] = std::move(renderTarget);
        created = true;
    }
    return created;
}

void TexturePool::removeStaleRenderTargets(const std::set<UnwrappedTileID>& currentTiles) {
    for (auto it = renderTargets.begin(); it != renderTargets.end();) {
        if (!currentTiles.contains(it->first)) {
            it = renderTargets.erase(it);
        } else {
            ++it;
        }
    }
}

std::shared_ptr<RenderTarget> TexturePool::getRenderTarget(const UnwrappedTileID& id) const {
    return renderTargets.contains(id) ? renderTargets.at(id) : nullptr;
}

std::shared_ptr<RenderTarget> TexturePool::getRenderTargetAncestorOrDescendant(
    const UnwrappedTileID& id, std::optional<UnwrappedTileID>& terrainTileID) const {
    terrainTileID = std::nullopt;
    std::shared_ptr<RenderTarget> bestRenderTarget;
    for (const auto& [tileID, renderTarget] : renderTargets) {
        if (tileID == id || tileID.isChildOf(id) || id.isChildOf(tileID)) {
            if (!terrainTileID || tileID.canonical.z > terrainTileID->canonical.z) {
                terrainTileID = tileID;
                bestRenderTarget = renderTarget;
            }
        }
    }
    return bestRenderTarget;
}
} // namespace mln
