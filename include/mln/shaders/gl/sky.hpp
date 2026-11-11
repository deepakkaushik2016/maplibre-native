// Generated code, do not modify this file!
#pragma once
#include <mln/shaders/shader_source.hpp>

namespace mln {
namespace shaders {

template <>
struct ShaderSource<BuiltIn::SkyShader, gfx::Backend::Type::OpenGL> {
    static constexpr const char* name = "SkyShader";
    static constexpr const char* vertex =
        R"(// The sky above the horizon line, after maplibre-gl-js sky.vertex.glsl. Rather than a full-screen
// quad that tests every fragment against the horizon, the quad is built on the horizon line itself
// (see SkyPropsUBO), so only fragments above the line are drawn, and the distance above the line
// interpolates linearly from the vertices.
layout (location = 0) in vec2 a_pos;

layout (std140) uniform SkyPropsUBO {
    highp vec4 u_sky_color;
    highp vec4 u_horizon_color;
    highp vec2 u_horizon;
    highp vec2 u_horizon_along;
    highp vec2 u_horizon_up;
    highp float u_sky_extent;
    highp float u_sky_horizon_blend;
};

out highp float v_blend;

void main() {
    gl_Position = vec4(u_horizon + (2.0 * a_pos.x - 1.0) * u_horizon_along + a_pos.y * u_horizon_up, 0.0, 1.0);
    v_blend = a_pos.y * u_sky_extent;
}
)";
    static constexpr const char* fragment = R"(layout (std140) uniform SkyPropsUBO {
    highp vec4 u_sky_color;
    highp vec4 u_horizon_color;
    highp vec2 u_horizon;
    highp vec2 u_horizon_along;
    highp vec2 u_horizon_up;
    highp float u_sky_extent;
    highp float u_sky_horizon_blend;
};

// Pixels above the horizon line.
in highp float v_blend;

void main() {
    // maplibre-gl-js sky.fragment.glsl, Mercator: the zenith colour, mixed towards the horizon
    // colour over the sky-horizon-blend band above the horizon line.
    if (v_blend < u_sky_horizon_blend) {
        fragColor = mix(u_sky_color, u_horizon_color, pow(1.0 - v_blend / u_sky_horizon_blend, 2.0));
    } else {
        fragColor = u_sky_color;
    }

#ifdef OVERDRAW_INSPECTOR
    fragColor = vec4(1.0);
#endif
}
)";
};

} // namespace shaders
} // namespace mln
