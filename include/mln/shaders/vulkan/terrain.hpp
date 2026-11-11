#pragma once

#include <mln/shaders/shader_source.hpp>
#include <mln/shaders/vulkan/shader_program.hpp>

namespace mln {
namespace shaders {

constexpr auto terrainShaderPrelude = R"(

#define idTerrainDrawableUBO        idDrawableReservedVertexOnlyUBO
#define idTerrainTilePropsUBO       idDrawableReservedFragmentOnlyUBO
#define idTerrainEvaluatedPropsUBO  layerUBOStartId

)";

template <>
struct ShaderSource<BuiltIn::TerrainShader, gfx::Backend::Type::Vulkan> {
    static constexpr const char* name = "TerrainShader";

    static const std::array<AttributeInfo, 1> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 2> textures;

    static constexpr auto prelude = terrainShaderPrelude;
    static constexpr auto vertex = R"(

layout(location = 0) in ivec4 in_position; // xy = tile position, z = skirt flag (1 = skirt)

layout(push_constant) uniform Constants {
    int ubo_index;
} constant;

struct TerrainDrawableUBO {
    mat4 matrix;
    vec4 dem_coords;
};

layout(std140, set = LAYER_SET_INDEX, binding = idTerrainDrawableUBO) readonly buffer TerrainDrawableUBOVector {
    TerrainDrawableUBO drawable_ubo[];
} drawableVector;

layout(set = LAYER_SET_INDEX, binding = idTerrainEvaluatedPropsUBO) uniform TerrainEvaluatedPropsUBO {
    vec4 unpack;
    float exaggeration;
    float elevation_offset;
    float fog_ground_blend;
    float fog_ground_blend_opacity;
    vec4 fog_color;
    vec4 horizon_color;
    float horizon_fog_blend;
    float fog_start;
    float fog_end;
    float pad1;
} props;

layout(set = DRAWABLE_IMAGE_SET_INDEX, binding = 0) uniform sampler2D dem_sampler;

layout(location = 0) out vec2 frag_uv;
layout(location = 1) out float frag_elevation;
layout(location = 2) out float frag_fog_depth;

void main() {
    const TerrainDrawableUBO drawable = drawableVector.drawable_ubo[constant.ubo_index];

    // The mesh was generated with coordinates from 0 to EXTENT (8192)
    const vec2 pos = vec2(in_position.xy);
    frag_uv = pos / 8192.0;

    // Decode the DEM and interpolate in meters via the shared helper (the packed
    // Terrain-RGB/Terrarium DEM cannot be hardware-filtered, so it is sampled
    // NEAREST and interpolated after decoding, matching maplibre-gl-js and the
    // elevated layers). Map into the bound DEM tile; an ancestor tile is bound as
    // a fallback while this tile's own DEM loads. dem_coords.w = DEM dimension.
    frag_elevation = get_elevation(pos, dem_sampler, drawable.dem_coords, props.unpack,
                                   drawable.dem_coords.w, props.exaggeration, 1.0);

    // Skirt vertices hang below the surface by elevation_offset, forming a curtain
    // that hides the cracks between neighbouring tiles at different zoom levels
    // (maplibre-gl-js u_ele_delta). in_position.z carries the skirt flag.
    const float ele_delta = in_position.z == 1 ? props.elevation_offset : 0.0;
    gl_Position = drawable.matrix * vec4(pos.x, pos.y, frag_elevation - ele_delta, 1.0);

    // maplibre-gl-js v_fog_depth: the depth of the surface point from the camera, its clip w (a skirt
    // vertex takes the surface's: w is linear in the height), mapped from [fog_start, fog_end] to
    // [0, 1] the way the perspective of gl-js's u_fog_matrix maps its z.
    const float fog_w = gl_Position.w + ele_delta * drawable.matrix[2][3];
    frag_fog_depth = props.fog_end * (fog_w - props.fog_start) / ((props.fog_end - props.fog_start) * fog_w);

    applySurfaceTransform();
}
)";

    static constexpr auto fragment = R"(

layout(location = 0) in vec2 frag_uv;
layout(location = 1) in float frag_elevation;
layout(location = 2) in float frag_fog_depth;
layout(location = 0) out vec4 out_color;

layout(set = LAYER_SET_INDEX, binding = idTerrainEvaluatedPropsUBO) uniform TerrainEvaluatedPropsUBO {
    vec4 unpack;
    float exaggeration;
    float elevation_offset;
    float fog_ground_blend;
    float fog_ground_blend_opacity;
    vec4 fog_color;
    vec4 horizon_color;
    float horizon_fog_blend;
    float fog_start;
    float fog_end;
    float pad1;
} props;

layout(set = DRAWABLE_IMAGE_SET_INDEX, binding = 1) uniform sampler2D map_sampler;

const float gamma = 2.2;

vec4 gammaToLinear(vec4 color) {
    return pow(color, vec4(gamma));
}

vec4 linearToGamma(vec4 color) {
    return pow(color, vec4(1.0 / gamma));
}

void main() {
    // Sample the map texture (render-to-texture output) for the surface color
    // Note: Y-coordinate is flipped (1.0 - y) to match OpenGL convention
    const vec4 surface_color = texture(map_sampler, vec2(frag_uv.x, 1.0 - frag_uv.y));

    // maplibre-gl-js 6.16.0 terrain.fragment.glsl: fade the distance into the fog colour, which
    // turns into the horizon colour near the far plane. Applied to the drape sample, so the drape
    // targets do not depend on the fog.
    if (props.fog_ground_blend_opacity > 0.0 && frag_fog_depth > props.fog_ground_blend) {
        const vec4 surface_color_linear = gammaToLinear(surface_color);
        const float blend_color = smoothstep(0.0, 1.0, max((frag_fog_depth - props.horizon_fog_blend) / (1.0 - props.horizon_fog_blend), 0.0));
        const vec4 fog_horizon_color_linear = mix(gammaToLinear(props.fog_color), gammaToLinear(props.horizon_color), blend_color);
        // gl-js clips at the fog's far plane (fog depth 1); native draws terrain past it, so there the
        // factor stops at 1, fully fogged, instead of overshooting the fog colour. In front of the far
        // plane the fog depth is above fog-ground-blend in this branch, so this is gl-js's
        // max(depth - blend, 0) / (1 - blend).
        const float factor_fog = min((frag_fog_depth - props.fog_ground_blend) / (1.0 - props.fog_ground_blend), 1.0);
        out_color = linearToGamma(mix(surface_color_linear, fog_horizon_color_linear, pow(factor_fog, 2.0) * props.fog_ground_blend_opacity));
    } else {
        out_color = surface_color;
    }

#if defined(OVERDRAW_INSPECTOR)
    out_color = vec4(1.0);
#endif
}
)";
};

} // namespace shaders
} // namespace mln
