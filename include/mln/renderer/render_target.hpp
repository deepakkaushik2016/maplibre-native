#pragma once

#include <mln/gfx/types.hpp>
#include <mln/renderer/terrain_cache_policy.hpp>
#include <mln/shaders/layer_ubo.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/util/size.hpp>
#include <mln/util/color.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

namespace mln {

namespace gfx {
class Context;
class Drawable;
class Texture2D;
class OffscreenTexture;
class UniformBuffer;
class UploadPass;
using Texture2DPtr = std::shared_ptr<Texture2D>;
using UniformBufferPtr = std::shared_ptr<UniformBuffer>;
} // namespace gfx

namespace shaders {
struct GlobalPaintParamsUBO;
} // namespace shaders

class LayerGroupBase;
class PaintParameters;
class RenderOrchestrator;
class RenderTree;

using LayerGroupBasePtr = std::shared_ptr<LayerGroupBase>;

/// Render target class
class RenderTarget {
public:
    RenderTarget(gfx::Context& context, const Size size, const gfx::TextureChannelDataType type, bool stencil);
    /// A target drawing into the offscreen texture of one that is gone, which keeps its GPU
    /// memory: no texture, buffer or framebuffer is made for it.
    RenderTarget(gfx::Context& context, std::unique_ptr<gfx::OffscreenTexture> offscreenTexture);
    ~RenderTarget();

    /// Set the color the render target is cleared to before its layer groups are drawn. A drape
    /// target is cleared to the map's solid background (the background layer drawn as a colour),
    /// so a new colour must re-render its cached drape.
    void setClearColor(const Color& color) {
        if (color != backgroundColor) {
            backgroundColor = color;
            clearColorChanged = true;
        }
    }

    /// Mark this render target as a terrain drape target for the given tile.
    /// Drape targets render the orchestrator's draped tile layer groups
    /// (filtered to overlapping tiles) instead of their own layer groups.
    void setDrapeTileID(const UnwrappedTileID& id);
    const std::optional<UnwrappedTileID>& getDrapeTileID() const { return drapeTileID; }

    /// Refresh this drape target's copy of the global paint parameters,
    /// carrying the target tile in `drape_tile` (no-op for non-drape targets).
    /// Only the target's own draws read the copy, so it reaches the GPU in render(),
    /// and only when the target re-renders: a drape kept from an earlier frame (every
    /// target while panning) costs no buffer update.
    void updateDrapeGlobalUBO(const shaders::GlobalPaintParamsUBO& params);

    /// Get the render target texture
    const gfx::Texture2DPtr& getTexture();

    /// Hands over the offscreen texture, for a new target to draw into. The target can't draw
    /// afterwards; only its last owner may take it.
    std::unique_ptr<gfx::OffscreenTexture> takeOffscreenTexture() { return std::move(offscreenTexture); }

    /// @brief Add a layer group to the render target
    /// @param replace Flag to replace if exists
    /// @return whether added
    bool addLayerGroup(LayerGroupBasePtr, bool replace);

    /// @brief Remove a layer group
    /// @param layerIndex index of the layer to remove
    /// @return whether removed
    bool removeLayerGroup(const int32_t layerIndex);

    /// Get the layer group count
    size_t numLayerGroups() const noexcept;

    /// @brief  Get a specific layer group by index
    /// @param layerIndex index
    /// @return the layer group if existent, otherwise a shared null pointer
    const LayerGroupBasePtr& getLayerGroup(const int32_t layerIndex) const;

    /// Execute the given function for each contained layer group
    template <typename Func /* void(LayerGroupBase&) */>
    void visitLayerGroups(Func f) {
        for (auto& pair : layerGroupsByLayerIndex) {
            if (pair.second) {
                f(*pair.second);
            }
        }
    }

    /// Execute the given function for each contained layer group in reversed order
    template <typename Func /* void(LayerGroupBase&) */>
    void visitLayerGroupsReversed(Func f) {
        for (auto& rit : std::ranges::reverse_view(layerGroupsByLayerIndex)) {
            if (rit.second) {
                f(*rit.second);
            }
        }
    }

    /// Upload the layer groups
    void upload(gfx::UploadPass& uploadPass);

    /// Outcome of a render() call, used by the drape render budget.
    enum class RenderResult {
        Skipped,  ///< Nothing to do (cache hit / render-once already baked).
        Rendered, ///< The target was (re-)rendered this frame (consumes drape budget).
        Deferred, ///< A re-render was needed but skipped for budget; keeps the stale texture.
    };

    /// Render the layer groups. When `canRerender` is false, a drape target that would
    /// re-render but already has a baked texture is deferred (returns Deferred) instead,
    /// so a burst of dirty drape targets is spread across frames rather than stalling one.
    RenderResult render(RenderOrchestrator&, const RenderTree&, PaintParameters&, bool canRerender = true);

protected:
    void renderDrapedLayerGroups(RenderOrchestrator&, PaintParameters&);

    /// What the draped layers would currently render into this target: how well
    /// they cover it, and a signature of the exact content
    struct DrapeCoverage {
        int32_t totalGroups = -1; // draped layer groups in the style
        /// How each draped layer group covers this target (in layer order): exact/deeper
        /// tiles, an ancestor n levels up, or nothing - see terrain::DrapeGroupFallback
        std::vector<terrain::DrapeGroupFallback> groups;

        /// Hash of the covering tile ids overlapping this target, each with its source bucket's
        /// id (summed, order-independent), so it changes only when the SET of covering tiles or
        /// their data changes - a tile loading, unloading or getting new data (a GeoJSON source
        /// update), or an integer-zoom crossing. Deliberately NOT keyed on drawable-instance
        /// ids: those are rebuilt with fresh ids every frame during fades/bucket updates, which
        /// churned the signature and re-rendered every drape each frame while panning.
        std::size_t contentHash = 0;
        /// Integer tile-zoom (draped UBOs carry zoom-derived values like line ratio);
        /// stored quantized so a pinch within one zoom level does not invalidate the
        /// cache. See computeDrapeCoverage.
        double zoom = -1;
        /// Evaluated-property generation. Retained for reference but intentionally NOT
        /// part of sameContentAs: the drape cache must not re-render on paint changes
        /// (matches maplibre-gl-js). See computeDrapeCoverage.
        uint64_t propertiesEpoch = 0;

        /// Whether this would draw exactly what `other` already did.
        /// The paint-property epoch is deliberately NOT part of this: any paint transition
        /// (e.g. a single label fading in) bumps the global epoch every frame, which defeated
        /// the drape cache entirely - all visible drape targets re-rendered every frame while
        /// panning (measured ~4x overdraw / 11fps -> 1x / 44fps once removed). gl-js keys its
        /// terrain RTT cache on tile coverage + zoom, never on paint changes.
        bool sameContentAs(const DrapeCoverage& other) const {
            return totalGroups == other.totalGroups && contentHash == other.contentHash && zoom == other.zoom;
        }
        /// Whether this would draw less than `other`: no layer group improved, and at least one
        /// lost its content or fell back to a coarser ancestor. Compared per group, not summed
        /// over groups - see terrain::drapeFallbackStrictlyWorse.
        bool worseThan(const DrapeCoverage& other) const {
            return terrain::drapeFallbackStrictlyWorse(groups, other.groups);
        }
    };
    DrapeCoverage computeDrapeCoverage(RenderOrchestrator&, const PaintParameters&) const;

    gfx::Context& context;
    std::unique_ptr<gfx::OffscreenTexture> offscreenTexture;
    using LayerGroupMap = std::map<int32_t, LayerGroupBasePtr>;
    LayerGroupMap layerGroupsByLayerIndex;
    Color backgroundColor;
    std::optional<UnwrappedTileID> drapeTileID;
    // (z, x including wrap, y, 1) of the drape tile, as consumed by
    // apply_drape_transform; w = 1 marks an active drape target
    std::array<float, 4> drapeTileValues{{0, 0, 0, 0}};
    gfx::UniformBufferPtr drapeGlobalUniformBuffer;
    // The values updateDrapeGlobalUBO last recorded, written to the buffer when the target renders
    shaders::GlobalPaintParamsUBO drapeGlobalParams{};
    // Whether updateDrapeGlobalUBO has recorded them: until it has, they are all zero, and a
    // drape rendered with them would draw nothing in the right place.
    bool drapeGlobalParamsRecorded = false;
    // Coverage baked into the target texture by the last actual render. The
    // target keeps its previously rendered content whenever the currently
    // available coverage is strictly worse, so a drape never regresses to
    // fewer layers / coarser fallbacks than it already shows (anti-flicker);
    // see RenderTarget::render.
    DrapeCoverage bakedCoverage;
    // The clear colour changed since the drape was last baked (setClearColor): re-render it even
    // though the drawables it covers did not change.
    bool clearColorChanged = false;
    // Opt-in "render once" for immutable targets (hillshade prepare, whose DEM input is
    // baked into the prepare drawable once). When set, the target renders on its first
    // frame and is skipped after (the offscreen texture persists). NOT set for the terrain
    // depth target, which must re-render whenever the camera moves. Enabled via
    // setRenderOnce() by RenderHillshadeLayer when it creates a prepare target.
    bool renderOnce = false;
    // Whether a render-once target has already produced its texture.
    bool renderedOnce = false;
    // Whether this drape target has been rendered at least once, so its offscreen texture
    // holds valid (if stale) content. Only such targets may be deferred by the drape budget;
    // a never-rendered target is always rendered to avoid showing a blank tile.
    bool hasRenderedContent = false;
    // Draw 3D drawables over the whole depth range [0, 1] rather than the [0, depthRangeSize]
    // that leaves room for 2D sublayers. Set for the terrain depth target, whose fragment
    // shaders pack the window depth for calculate_visibility to compare with clip-space NDC z.
    bool fullDepthRange = false;

public:
    void setRenderOnce(bool value) { renderOnce = value; }
    void setFullDepthRange(bool value) { fullDepthRange = value; }

    /// What a draped drawable's content is drawn from, folded into the drape signatures with its
    /// covering tile: its source bucket's id, or, for the hillshade and color-relief layers
    /// (whose drawables hold no bucket), the texture they sample the DEM through. Those are made
    /// anew when the tile's DEM changes - a reload, or a border backfilled from a neighbour that
    /// arrived later - so the drape is re-rendered then, and only then. Also folded in: the
    /// bucket's feature-state revision (paint attributes rewritten in place under the same id) and
    /// the layer group's paint revision (a style edit, a running transition or a global-state
    /// change), so a paint change re-renders the drape targets the layer covers on the next frame.
    /// O(1).
    static std::size_t drapedSourceHash(const LayerGroupBase&, const gfx::Drawable&);

protected:
    // This target's own content signature (PaintParameters::perTargetDrapeSignature)
    // as of the last frame it evaluated its coverage: a signature of just the
    // drawables overlapping this target, plus zoom and the property epoch. While it
    // is unchanged, nothing this target draws has changed, so its baked texture is
    // still correct and the O(draped drawables) coverage scan is skipped. Being
    // per-target (not global), an unrelated tile loading elsewhere no longer forces
    // this target to re-scan. Unset until the target has been evaluated once.
    std::optional<std::size_t> bakedSignature;
};

} // namespace mln
