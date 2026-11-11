#pragma once

#include <mln/style/sky_impl.hpp>
#include <mln/util/immutable.hpp>

#include <array>
#include <memory>
#include <optional>

namespace mln {

class TransformState;
class TransitionParameters;
class PropertyEvaluationParameters;
class LayerGroupBase;
using LayerGroupBasePtr = std::shared_ptr<LayerGroupBase>;

namespace gfx {
class Context;
class ShaderRegistry;
} // namespace gfx

using TransitioningSky = style::SkyProperties::Unevaluated;
using EvaluatedSky = style::SkyProperties::PossiblyEvaluated;

/// The style's sky, drawn as a gradient above the horizon line before anything else in the frame,
/// as maplibre-gl-js draws it (`drawSky`, Mercator): from `horizon-color` at the horizon line to
/// `sky-color` over the `sky-horizon-blend` band. The map's layers and the terrain draw over it.
///
/// The sky's layer group is not registered with the orchestrator: Renderer::Impl uploads it and
/// draws it right after clearing the main buffer. `fog-color`, `fog-ground-blend` and
/// `horizon-fog-blend` colour the terrain, not the sky; `atmosphere-blend` applies to globe only.
class RenderSky {
public:
    explicit RenderSky(Immutable<style::Sky::Impl>);
    ~RenderSky();

    void transition(const TransitionParameters&);
    void evaluate(const PropertyEvaluationParameters&);
    bool hasTransition() const;

    const EvaluatedSky& getEvaluated() const;

    /// Where the sky goes in the viewport, in clip space (see shaders::SkyPropsUBO).
    struct Quad {
        /// A point on the horizon line.
        std::array<float, 2> horizon;
        /// Along the horizon line from `horizon`, far enough to reach past the viewport.
        std::array<float, 2> along;
        /// Up from the horizon line, `extent` pixels long.
        std::array<float, 2> up;
        float extent;
    };

    /// The sky's quad for this camera, or std::nullopt when no part of the viewport is above the
    /// horizon line, so there is no sky to draw. The horizon line follows maplibre-gl-js
    /// `getMercatorHorizon` and `skyUniformValues`: above the centre of the viewport by
    /// cameraToCenterDistance * min(0.85 * tan(90° - pitch), tan(89.25° - pitch)), turned by the
    /// roll.
    static std::optional<Quad> quad(const TransformState&);

    /// Builds the sky drawable once, and sets this frame's uniforms; disables the layer group when
    /// quad() gives no sky, so no sky draw is issued.
    void update(gfx::ShaderRegistry&, gfx::Context&, const TransformState&);

    const LayerGroupBasePtr& getLayerGroup() const { return layerGroup; }

    Immutable<style::Sky::Impl> impl;

private:
    TransitioningSky transitioning;
    EvaluatedSky evaluated;
    LayerGroupBasePtr layerGroup;
};

} // namespace mln
