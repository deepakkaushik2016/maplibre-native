#pragma once

#include <mln/shaders/shader_source.hpp>
#include <mln/shaders/vulkan/shader_program.hpp>

namespace mln {
namespace shaders {

constexpr auto skyShaderPrelude = R"(#define idSkyPropsUBO    layerUBOStartId)";

template <>
struct ShaderSource<BuiltIn::SkyShader, gfx::Backend::Type::Vulkan> {
    static constexpr const char* name = "SkyShader";

    static const std::array<AttributeInfo, 1> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 0> textures;

    static constexpr auto prelude = skyShaderPrelude;
    static constexpr auto vertex = R"(

layout(location = 0) in ivec2 in_position;

layout(set = LAYER_SET_INDEX, binding = idSkyPropsUBO) uniform SkyPropsUBO {
    vec4 sky_color;
    vec4 horizon_color;
    vec2 horizon;
    vec2 horizon_along;
    vec2 horizon_up;
    float sky_extent;
    float sky_horizon_blend;
} props;

layout(location = 0) out float frag_blend;

void main() {
    const vec2 pos = vec2(in_position);
    gl_Position = vec4(props.horizon + (2.0 * pos.x - 1.0) * props.horizon_along + pos.y * props.horizon_up, 0.0, 1.0);
    applySurfaceTransform();

    frag_blend = pos.y * props.sky_extent;
}
)";

    static constexpr auto fragment = R"(

layout(location = 0) in float frag_blend;
layout(location = 0) out vec4 out_color;

layout(set = LAYER_SET_INDEX, binding = idSkyPropsUBO) uniform SkyPropsUBO {
    vec4 sky_color;
    vec4 horizon_color;
    vec2 horizon;
    vec2 horizon_along;
    vec2 horizon_up;
    float sky_extent;
    float sky_horizon_blend;
} props;

void main() {

#if defined(OVERDRAW_INSPECTOR)
    out_color = vec4(1.0);
    return;
#endif

    if (frag_blend < props.sky_horizon_blend) {
        out_color = mix(props.sky_color, props.horizon_color, pow(1.0 - frag_blend / props.sky_horizon_blend, 2.0));
    } else {
        out_color = props.sky_color;
    }
}
)";
};

} // namespace shaders
} // namespace mln
