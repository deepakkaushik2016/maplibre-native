#pragma once

#include <mln/tile/tile_id.hpp>
#include <mln/renderer/render_target.hpp>
#include <optional>
#include <set>

namespace mln {
class TexturePool {
public:
    TexturePool(uint32_t tilesize);
    ~TexturePool();

    std::shared_ptr<RenderTarget> getRenderTarget(const UnwrappedTileID& id) const;
    std::shared_ptr<RenderTarget> getRenderTargetAncestorOrDescendant(
        const UnwrappedTileID& id, std::optional<UnwrappedTileID>& terrainTileID) const;
    /// The drape target of tile `id`, created unless it exists; returns whether it was created.
    bool createRenderTarget(gfx::Context& context, const UnwrappedTileID& id, const Color& backgroundColor);

    /// Makes the drape targets those of `cover`: drops the targets of tiles that left it and
    /// creates targets for tiles that joined it. Returns whether a target was created.
    bool updateRenderTargets(gfx::Context& context,
                             const std::set<UnwrappedTileID>& cover,
                             const Color& backgroundColor);

    /// Remove render targets for tiles that are no longer part of the given set
    void removeStaleRenderTargets(const std::set<UnwrappedTileID>& currentTiles);

    template <typename Func /* void(std::shared_ptr<RenderTarget>&) */>
    void visitRenderTargets(Func f) {
        for (auto& pair : renderTargets) {
            if (pair.second) {
                f(pair.second);
            }
        }
    }

    template <typename Func /* void(std::shared_ptr<RenderTarget>&) */>
    void visitRenderTargets(Func f) const {
        for (const auto& pair : renderTargets) {
            if (pair.second) {
                f(pair.second);
            }
        }
    }

private:
    uint32_t tileSize;

    std::map<UnwrappedTileID, std::shared_ptr<RenderTarget>> renderTargets;
};

} // namespace mln
