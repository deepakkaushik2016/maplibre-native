#include <mln/renderer/render_sky.hpp>

#include <mln/gfx/context.hpp>
#include <mln/gfx/color_mode.hpp>
#include <mln/gfx/cull_face_mode.hpp>
#include <mln/gfx/drawable.hpp>
#include <mln/gfx/drawable_builder.hpp>
#include <mln/map/transform_state.hpp>
#include <mln/math/clamp.hpp>
#include <mln/renderer/layer_group.hpp>
#include <mln/renderer/property_evaluation_parameters.hpp>
#include <mln/renderer/render_pass.hpp>
#include <mln/renderer/render_static_data.hpp>
#include <mln/renderer/transition_parameters.hpp>
#include <mln/shaders/shader_defines.hpp>
#include <mln/shaders/sky_layer_ubo.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/logging.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace mln {

using namespace shaders;

namespace {

/// maplibre-gl-js `maxMercatorHorizonAngle`.
constexpr double maxMercatorHorizonAngle = 89.25 * std::numbers::pi / 180.0;

} // namespace

RenderSky::RenderSky(Immutable<style::Sky::Impl> impl_)
    : impl(std::move(impl_)),
      transitioning(impl->properties.untransitioned()) {}

RenderSky::~RenderSky() = default;

void RenderSky::transition(const TransitionParameters& parameters) {
    transitioning = impl->properties.transitioned(parameters, std::move(transitioning));
}

void RenderSky::evaluate(const PropertyEvaluationParameters& parameters) {
    evaluated = transitioning.evaluate(parameters);
}

bool RenderSky::hasTransition() const {
    return transitioning.hasTransition();
}

const EvaluatedSky& RenderSky::getEvaluated() const {
    return evaluated;
}

std::optional<RenderSky::Quad> RenderSky::quad(const TransformState& state) {
    const Size size = state.getSize();
    if (size.isEmpty()) {
        return std::nullopt;
    }
    const double width = size.width;
    const double height = size.height;

    // In pixels with y up from the bottom of the viewport, as gl_FragCoord in maplibre-gl-js.
    const double pitch = state.getPitch();
    const double horizonAboveCenter = state.getCameraToCenterDistance() *
                                      std::min(std::tan(std::numbers::pi / 2.0 - pitch) * 0.85,
                                               std::tan(maxMercatorHorizonAngle - pitch));
    const double cosRoll = std::cos(state.getRoll());
    const double sinRoll = std::sin(state.getRoll());
    const std::array<double, 2> horizon = {width / 2.0 - horizonAboveCenter * sinRoll,
                                           height / 2.0 + horizonAboveCenter * cosRoll};
    const std::array<double, 2> normal = {-sinRoll, cosRoll};

    double highestAbove = -INFINITY;
    for (const auto& [x, y] : {std::array<double, 2>{0, 0}, {width, 0}, {0, height}, {width, height}}) {
        highestAbove = std::max(highestAbove, (x - horizon[0]) * normal[0] + (y - horizon[1]) * normal[1]);
    }
    if (highestAbove <= 0.0) {
        return std::nullopt;
    }

    // Every point of the viewport lies within `reach` of the horizon point.
    const double reach = std::hypot(horizon[0] - width / 2.0, horizon[1] - height / 2.0) + width + height;
    const auto toClip = [&](double dx, double dy) {
        return std::array<float, 2>{static_cast<float>(dx * 2.0 / width), static_cast<float>(dy * 2.0 / height)};
    };
    const auto horizonClip = toClip(horizon[0], horizon[1]);
    return Quad{.horizon = {horizonClip[0] - 1.0f, horizonClip[1] - 1.0f},
                .along = toClip(cosRoll * reach, sinRoll * reach),
                .up = toClip(normal[0] * reach, normal[1] * reach),
                .extent = static_cast<float>(reach)};
}

void RenderSky::update(gfx::ShaderRegistry& shaders, gfx::Context& context, const TransformState& state) {
    if (!layerGroup) {
        layerGroup = context.createLayerGroup(/*layerIndex=*/0, /*initialCapacity=*/1, "sky", false);
        if (!layerGroup) {
            return;
        }
    }

    const auto skyQuad = quad(state);
    layerGroup->setEnabled(skyQuad.has_value());
    if (!skyQuad) {
        return;
    }

    if (layerGroup->empty()) {
        const auto shader = context.getGenericShader(shaders, "SkyShader");
        if (!shader) {
            Log::Error(Event::Render, "Sky shader not found");
            return;
        }

        // (0|1, 0|1): along the horizon line, and up from it (shaders::SkyPropsUBO).
        constexpr std::array<int16_t, 8> corners = {0, 0, 1, 0, 0, 1, 1, 1};
        std::vector<uint8_t> vertices(sizeof(corners));
        std::memcpy(vertices.data(), corners.data(), sizeof(corners));
        SegmentVector segments;
        segments.emplace_back(0, 0, 4, 6);

        auto builder = context.createDrawableBuilder("sky");
        builder->setShader(shader);
        builder->setRenderPass(RenderPass::Opaque);
        builder->setEnableDepth(false);
        builder->setColorMode(gfx::ColorMode::alphaBlended());
        builder->setCullFaceMode(gfx::CullFaceMode::disabled());
        builder->setVertexAttrId(idSkyPosVertexAttribute);
        builder->setRawVertices(std::move(vertices), 4, gfx::AttributeDataType::Short2);
        builder->setSegments(
            gfx::Triangles(), RenderStaticData::quadTriangleIndices().vector(), segments.data(), segments.size());
        builder->flush(context);
        for (auto& drawable : builder->clearDrawables()) {
            static_cast<LayerGroup&>(*layerGroup).addDrawable(std::move(drawable));
        }
    }

    const SkyPropsUBO propsUBO = {
        .sky_color = evaluated.get<style::SkySkyColor>(),
        .horizon_color = evaluated.get<style::SkyHorizonColor>(),
        .horizon = skyQuad->horizon,
        .horizon_along = skyQuad->along,
        .horizon_up = skyQuad->up,
        .sky_extent = skyQuad->extent,
        .sky_horizon_blend = util::clamp(evaluated.get<style::SkySkyHorizonBlend>(), 0.0f, 1.0f) *
                             static_cast<float>(state.getSize().height) / 2.0f,
    };
    layerGroup->mutableUniformBuffers().createOrUpdate(idSkyPropsUBO, &propsUBO, context);
}

} // namespace mln
