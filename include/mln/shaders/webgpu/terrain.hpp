#pragma once

#include <mln/shaders/shader_source.hpp>
#include <mln/shaders/webgpu/shader_program.hpp>
#include <mln/shaders/terrain_layer_ubo.hpp>

namespace mln {
namespace shaders {

template <>
struct ShaderSource<BuiltIn::TerrainShader, gfx::Backend::Type::WebGPU> {
    static constexpr const char* name = "TerrainShader";
    static const std::array<AttributeInfo, 1> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 2> textures;

    static constexpr auto vertex = R"(
struct VertexInput {
    @location(5) position: vec4<i32>, // xy = tile position, z = skirt flag (1 = skirt)
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) elevation: f32,
    @location(2) fog_depth: f32,
};

struct GlobalIndexUBO {
    value: u32,
    pad0: vec3<u32>,
};

struct TerrainDrawableUBO {
    matrix: mat4x4<f32>,
    dem_coords: vec4<f32>,
};

struct TerrainEvaluatedPropsUBO {
    unpack: vec4<f32>,
    exaggeration: f32,
    elevation_offset: f32,
    fog_ground_blend: f32,
    fog_ground_blend_opacity: f32,
    fog_color: vec4<f32>,
    horizon_color: vec4<f32>,
    horizon_fog_blend: f32,
    fog_start: f32,
    fog_end: f32,
    pad1: f32,
};

@group(0) @binding(1) var<uniform> globalIndex: GlobalIndexUBO;
@group(0) @binding(2) var<storage, read> drawableVector: array<TerrainDrawableUBO>;
@group(0) @binding(4) var<uniform> props: TerrainEvaluatedPropsUBO;
@group(1) @binding(0) var texture_sampler: sampler;
@group(1) @binding(1) var dem_texture: texture_2d<f32>;

@vertex
fn main(in: VertexInput) -> VertexOutput {
    var out: VertexOutput;
    let drawable = drawableVector[globalIndex.value];

    // The mesh was generated with coordinates from 0 to EXTENT (8192)
    let pos = vec2<f32>(f32(in.position.x), f32(in.position.y));
    let uv = pos / 8192.0;

    // Decode the DEM and interpolate in meters via the shared helper (the packed
    // Terrain-RGB/Terrarium DEM cannot be hardware-filtered, so it is sampled
    // NEAREST and interpolated after decoding, matching maplibre-gl-js and the
    // elevated layers). Map into the bound DEM tile; an ancestor tile is bound as
    // a fallback while this tile's own DEM loads. dem_coords.w = DEM dimension.
    let elevation = get_elevation(pos, dem_texture, texture_sampler, drawable.dem_coords, props.unpack,
                                  drawable.dem_coords.w, props.exaggeration, 1.0);

    // Skirt vertices hang below the surface by elevation_offset, forming a curtain
    // that hides the cracks between neighbouring tiles at different zoom levels
    // (maplibre-gl-js u_ele_delta). position.z carries the skirt flag.
    let ele_delta = select(0.0, props.elevation_offset, in.position.z == 1);
    out.position = drawable.matrix * vec4<f32>(pos.x, pos.y, elevation - ele_delta, 1.0);
    out.uv = uv;
    out.elevation = elevation;

    // maplibre-gl-js v_fog_depth: the depth of the surface point from the camera, its clip w (a skirt
    // vertex takes the surface's: w is linear in the height), mapped from [fog_start, fog_end] to
    // [0, 1] the way the perspective of gl-js's u_fog_matrix maps its z.
    let fog_w = out.position.w + ele_delta * drawable.matrix[2][3];
    out.fog_depth = props.fog_end * (fog_w - props.fog_start) / ((props.fog_end - props.fog_start) * fog_w);
    return out;
}
)";

    static constexpr auto fragment = R"(
struct FragmentInput {
    @location(0) uv: vec2<f32>,
    @location(1) elevation: f32,
    @location(2) fog_depth: f32,
};

struct TerrainEvaluatedPropsUBO {
    unpack: vec4<f32>,
    exaggeration: f32,
    elevation_offset: f32,
    fog_ground_blend: f32,
    fog_ground_blend_opacity: f32,
    fog_color: vec4<f32>,
    horizon_color: vec4<f32>,
    horizon_fog_blend: f32,
    fog_start: f32,
    fog_end: f32,
    pad1: f32,
};

@group(0) @binding(4) var<uniform> props: TerrainEvaluatedPropsUBO;
@group(1) @binding(0) var texture_sampler: sampler;
@group(1) @binding(2) var map_texture: texture_2d<f32>;

@fragment
fn main(in: FragmentInput) -> @location(0) vec4<f32> {
    // Sample the map texture (render-to-texture output) for the surface color. WebGPU's
    // NDC is Y-up, as Metal's is, so the drape target is stored upright and uv.y is
    // sampled as it is. Flipping it (1.0 - uv.y, as Vulkan, whose NDC is Y-down, must)
    // turns the draped map upside down about each mesh tile.
    let surface_color = textureSample(map_texture, texture_sampler, in.uv);

    // maplibre-gl-js 6.16.0 terrain.fragment.glsl: fade the distance into the fog colour, which
    // turns into the horizon colour near the far plane. Applied to the drape sample, so the drape
    // targets do not depend on the fog.
    if (props.fog_ground_blend_opacity > 0.0 && in.fog_depth > props.fog_ground_blend) {
        let gamma = vec4<f32>(2.2);
        let surface_color_linear = pow(surface_color, gamma);
        let blend_color = smoothstep(0.0, 1.0, max((in.fog_depth - props.horizon_fog_blend) / (1.0 - props.horizon_fog_blend), 0.0));
        let fog_horizon_color_linear = mix(pow(props.fog_color, gamma), pow(props.horizon_color, gamma), vec4<f32>(blend_color));
        // gl-js clips at the fog's far plane (fog depth 1); native draws terrain past it, so there the
        // factor stops at 1, fully fogged, instead of overshooting the fog colour. In front of the far
        // plane the fog depth is above fog-ground-blend in this branch, so this is gl-js's
        // max(depth - blend, 0) / (1 - blend).
        let factor_fog = min((in.fog_depth - props.fog_ground_blend) / (1.0 - props.fog_ground_blend), 1.0);
        return pow(mix(surface_color_linear, fog_horizon_color_linear, vec4<f32>(pow(factor_fog, 2.0) * props.fog_ground_blend_opacity)), 1.0 / gamma);
    }
    return surface_color;
}
)";
};

} // namespace shaders
} // namespace mln
