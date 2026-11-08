#pragma once

#include <mln/gfx/backend.hpp>
#include <mln/gfx/rendering_stats.hpp>
#include <mln/shaders/shader_source.hpp>
#include <mln/text/glyph_range.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/tile/tile_operation.hpp>
#include <mln/util/font_stack.hpp>
#include <mln/util/geo.hpp>
#include <mln/util/symbol_error_observer.hpp>

#include <cstdint>
#include <exception>
#include <functional>
#include <string>

namespace mln {

namespace gfx {
class ShaderRegistry;
}

class RendererObserver : public SymbolErrorObserver {
public:
    ~RendererObserver() override = default;

    enum class RenderMode : uint32_t {
        Partial,
        Full
    };

    /// Signals that a repaint is required
    virtual void onInvalidate() {}

    /// Resource failed to download / parse
    virtual void onResourceError(std::exception_ptr) {}

    /// First frame
    virtual void onWillStartRenderingMap() {}

    /// Start of frame, initial is the first frame for this map
    virtual void onWillStartRenderingFrame() {}

    /// End of frame, booleans flags that a repaint is required and that placement changed.
    virtual void onDidFinishRenderingFrame(RenderMode, bool /*repaint*/, bool /*placementChanged*/) {}

    /// End of frame, booleans flags that a repaint is required and that placement changed.
    virtual void onDidFinishRenderingFrame(RenderMode mode,
                                           bool repaint,
                                           bool placementChanged,
                                           double /*frameEncodingTime*/,
                                           double /*frameRenderingTime*/) {
        onDidFinishRenderingFrame(mode, repaint, placementChanged);
    }

    virtual void onDidFinishRenderingFrame(RenderMode mode,
                                           bool repaint,
                                           bool placementChanged,
                                           const gfx::RenderingStats& stats) {
        onDidFinishRenderingFrame(mode, repaint, placementChanged, stats.encodingTime, stats.renderingTime);
    }

    virtual void onDidFinishRenderingFrame(RenderMode mode,
                                           bool repaint,
                                           bool placementChanged,
                                           std::shared_ptr<gfx::RenderingStats> stats) {
        onDidFinishRenderingFrame(mode, repaint, placementChanged, *stats);
    }

    /// Final frame
    virtual void onDidFinishRenderingMap() {}

    /// The rendered terrain height under the map centre changed. Only the render side knows
    /// it - the DEM lives there - so a map that wants its centre to ride the terrain
    /// (Map::setCenterClampedToGround) learns of it here, one frame behind. `center` is the
    /// centre the frame drew, whose ground this is: the camera may have moved on since.
    virtual void onTerrainCenterElevationChanged(double /*elevationMeters*/, const LatLng& /*center*/) {}

    /// The rendered terrain height under the camera's eye changed (one frame behind, like the
    /// center's). A map that keeps its eye above the ground (Map::setCenterClampedToGround)
    /// learns of it here: at a steep pitch the eye sits only a little above the center, so a
    /// slope rising towards the camera puts the eye under the ground.
    virtual void onTerrainEyeElevationChanged(double /*elevationMeters*/) {}

    /// The terrain height at the center a running camera animation is headed for, once a DEM tile
    /// covers it (UpdateParameters::transitionTargetCenter), and whenever a finer one changes it.
    virtual void onTerrainTransitionTargetElevationChanged(double /*elevationMeters*/) {}

    /// The height of the ground a gesture grabbed (UpdateParameters::terrainGrab, `grabId`), where
    /// the line of sight it was grabbed along meets the rendered terrain. Reported once per grab,
    /// once a DEM tile covers that ground.
    virtual void onTerrainGrabbedGroundElevation(uint64_t /*grabId*/, double /*elevationMeters*/) {}

    /// Style is missing an image
    using StyleImageMissingCallback = std::function<void()>;
    virtual void onStyleImageMissing(const std::string&, const StyleImageMissingCallback& done) { done(); }
    virtual void onRemoveUnusedStyleImages(const std::vector<std::string>&) {}

    // Entry point for custom shader registration
    virtual void onRegisterShaders(gfx::ShaderRegistry&) {};
    virtual void onPreCompileShader(shaders::BuiltIn, gfx::Backend::Type, const std::string&) {}
    virtual void onPostCompileShader(shaders::BuiltIn, gfx::Backend::Type, const std::string&) {}
    virtual void onShaderCompileFailed(shaders::BuiltIn, gfx::Backend::Type, const std::string&) {}

    // Glyph loading
    virtual void onGlyphsLoaded(const FontStack&, const GlyphRange&) {}
    virtual void onGlyphsError(const FontStack&, const GlyphRange&, std::exception_ptr) {}
    virtual void onGlyphsRequested(const FontStack&, const GlyphRange&) {}

    // Tile loading
    virtual void onTileAction(TileOperation, const OverscaledTileID&, const std::string&) {}

    /// Render layer or drawable failed
    virtual void onRenderError(std::exception_ptr) {}
};

} // namespace mln
