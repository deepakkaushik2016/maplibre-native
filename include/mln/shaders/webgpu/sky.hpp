#pragma once

#include <mln/shaders/shader_source.hpp>
#include <mln/shaders/webgpu/shader_program.hpp>
#include <mln/shaders/sky_layer_ubo.hpp>

namespace mln {
namespace shaders {

template <>
struct ShaderSource<BuiltIn::SkyShader, gfx::Backend::Type::WebGPU> {
    static constexpr const char* name = "SkyShader";
    static const std::array<AttributeInfo, 1> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 0> textures;

    static constexpr auto vertex = R"(
struct VertexInput {
    @location(5) position: vec2<i32>,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) blend: f32,
};

struct SkyPropsUBO {
    sky_color: vec4<f32>,
    horizon_color: vec4<f32>,
    horizon: vec2<f32>,
    horizon_along: vec2<f32>,
    horizon_up: vec2<f32>,
    sky_extent: f32,
    sky_horizon_blend: f32,
};

@group(0) @binding(4) var<uniform> props: SkyPropsUBO;

@vertex
fn main(in: VertexInput) -> VertexOutput {
    var out: VertexOutput;
    let pos = vec2<f32>(f32(in.position.x), f32(in.position.y));
    out.position = vec4<f32>(props.horizon + (2.0 * pos.x - 1.0) * props.horizon_along + pos.y * props.horizon_up, 0.0, 1.0);
    out.blend = pos.y * props.sky_extent;
    return out;
}
)";

    static constexpr auto fragment = R"(
struct FragmentInput {
    @location(0) blend: f32,
};

struct SkyPropsUBO {
    sky_color: vec4<f32>,
    horizon_color: vec4<f32>,
    horizon: vec2<f32>,
    horizon_along: vec2<f32>,
    horizon_up: vec2<f32>,
    sky_extent: f32,
    sky_horizon_blend: f32,
};

@group(0) @binding(4) var<uniform> props: SkyPropsUBO;

@fragment
fn main(in: FragmentInput) -> @location(0) vec4<f32> {
    if (in.blend < props.sky_horizon_blend) {
        return mix(props.sky_color, props.horizon_color, pow(1.0 - in.blend / props.sky_horizon_blend, 2.0));
    }
    return props.sky_color;
}
)";
};

} // namespace shaders
} // namespace mln
