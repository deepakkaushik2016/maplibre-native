#include <mln/renderer/texture_pool.hpp>
#include <mln/gfx/context.hpp>
#include <mln/gfx/offscreen_texture.hpp>
#include <mln/util/logging.hpp>

#include <atomic>
#include <vector>

namespace mln {

namespace {
std::atomic<uint64_t> lentCount{0};
std::atomic<bool> lendingEnabled{true};
} // namespace

void TexturePool::setLendingEnabled(bool enabled) {
    lendingEnabled.store(enabled, std::memory_order_relaxed);
}

uint64_t TexturePool::texturesLent() {
    return lentCount.load(std::memory_order_relaxed);
}

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
    // drawn anew, as a new one is, since the new target has baked nothing yet. A target held
    // elsewhere, or whose texture is held elsewhere, or of another size or format, lends
    // nothing. What isn't lent this frame is freed, so no texture is kept back at rest.
    const RenderTarget::Spec drapeSpec{
        .size = {tileSize, tileSize}, .type = gfx::TextureChannelDataType::UnsignedByte, .stencil = true};
    std::vector<std::unique_ptr<gfx::OffscreenTexture>> spare;
    spare.reserve(renderTargets.size());
    for (auto it = renderTargets.begin(); it != renderTargets.end();) {
        if (cover.contains(it->first)) {
            ++it;
            continue;
        }
        // Out of the pool first, so a target that has lent its texture is never left in it.
        std::shared_ptr<RenderTarget> leaving = std::move(it->second);
        it = renderTargets.erase(it);
        // Only a target nothing else holds, whose texture nothing else holds either (the terrain
        // drawable of its tile samples it until RenderTerrain drops it), lends its texture.
        if (lendingEnabled.load(std::memory_order_relaxed) && leaving && leaving.use_count() == 1 &&
            leaving->canLend(drapeSpec)) {
            spare.push_back(leaving->takeOffscreenTexture());
        }
    }

    bool created = false;
    for (const auto& id : cover) {
        if (auto it = renderTargets.find(id); it != renderTargets.end() && it->second) {
            it->second->setClearColor(backgroundColor);
            continue;
        }
        std::shared_ptr<RenderTarget> renderTarget;
        if (!spare.empty()) {
            renderTarget = std::make_shared<RenderTarget>(context, std::move(spare.back()), drapeSpec);
            spare.pop_back();
            lentCount.fetch_add(1, std::memory_order_relaxed);
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
