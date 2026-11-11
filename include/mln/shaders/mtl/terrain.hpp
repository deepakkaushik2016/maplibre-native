#pragma once

#include <mln/shaders/terrain_layer_ubo.hpp>
#include <mln/shaders/shader_source.hpp>
#include <mln/shaders/mtl/shader_program.hpp>

namespace mln {
namespace shaders {

constexpr auto terrainShaderPrelude = R"(

enum {
    idTerrainDrawableUBO = idDrawableReservedVertexOnlyUBO,
    idTerrainTilePropsUBO = idDrawableReservedFragmentOnlyUBO,
    idTerrainEvaluatedPropsUBO = drawableReservedUBOCount,
    terrainUBOCount
};

struct alignas(16) TerrainDrawableUBO {
    /*  0 */ float4x4 matrix;
    /* 64 */ float4 dem_coords;
    /* 80 */
};
static_assert(sizeof(TerrainDrawableUBO) == 5 * 16, "wrong size");

struct alignas(16) TerrainTilePropsUBO {
    /*  0 */ float2 dem_tl;
    /*  8 */ float dem_scale;
    /* 12 */ float pad1;
    /* 16 */
};
static_assert(sizeof(TerrainTilePropsUBO) == 16, "wrong size");

/// Evaluated properties that do not depend on the tile, and the terrain fog
struct alignas(16) TerrainEvaluatedPropsUBO {
    /*  0 */ float4 unpack; // DEM unpack vector for the source's encoding
    /* 16 */ float exaggeration;
    /* 20 */ float elevation_offset;
    /* 24 */ float fog_ground_blend;
    /* 28 */ float fog_ground_blend_opacity;
    /* 32 */ float4 fog_color;
    /* 48 */ float4 horizon_color;
    /* 64 */ float horizon_fog_blend;
    /* 68 */ float fog_start;
    /* 72 */ float fog_end;
    /* 76 */ float pad1;
    /* 80 */
};
static_assert(sizeof(TerrainEvaluatedPropsUBO) == 5 * 16, "wrong size");

)";

template <>
struct ShaderSource<BuiltIn::TerrainShader, gfx::Backend::Type::Metal> {
    static constexpr auto name = "TerrainShader";
    static constexpr auto vertexMainFunction = "vertexMain";
    static constexpr auto fragmentMainFunction = "fragmentMain";

    static const std::array<AttributeInfo, 1> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 2> textures;

    static constexpr auto prelude = terrainShaderPrelude;
    static constexpr auto source = R"(

struct VertexStage {
    short4 pos [[attribute(0)]]; // xy = tile position, z = skirt flag (1 = skirt)
};

struct FragmentStage {
    float4 position [[position, invariant]];
    float2 uv;
    float elevation;
    float fog_depth;
};

FragmentStage vertex vertexMain(thread const VertexStage vertx [[stage_in]],
                                device const uint32_t& uboIndex [[buffer(idGlobalUBOIndex)]],
                                device const TerrainDrawableUBO* drawableVector [[buffer(idTerrainDrawableUBO)]],
                                device const TerrainEvaluatedPropsUBO& props [[buffer(idTerrainEvaluatedPropsUBO)]],
                                texture2d<float, access::sample> demTexture [[texture(0)]],
                                sampler demSampler [[sampler(0)]]) {

    device const TerrainDrawableUBO& drawable = drawableVector[uboIndex];

    // The mesh was generated with coordinates from 0 to EXTENT (8192)
    float2 pos = float2(vertx.pos.xy);
    float2 uv = pos / 8192.0;

    // Decode the DEM and interpolate in meters via the shared helper (the packed
    // Terrain-RGB/Terrarium DEM cannot be hardware-filtered, so it is sampled
    // NEAREST and interpolated after decoding, matching maplibre-gl-js and the
    // elevated layers). Map into the bound DEM tile; an ancestor tile is bound as
    // a fallback while this tile's own DEM loads. dem_coords.w = DEM dimension.
    float elevation = get_elevation(pos, demTexture, demSampler, drawable.dem_coords, props.unpack,
                                    drawable.dem_coords.w, props.exaggeration, 1.0);

    // Skirt vertices hang below the surface by elevation_offset, forming a curtain
    // that hides the cracks between neighbouring tiles at different zoom levels
    // (maplibre-gl-js u_ele_delta). pos.z carries the skirt flag.
    const float ele_delta = (float(vertx.pos.z) == 1.0) ? props.elevation_offset : 0.0;
    float4 position = drawable.matrix * float4(pos.x, pos.y, elevation - ele_delta, 1.0);

    // maplibre-gl-js v_fog_depth: the depth of the surface point from the camera, its clip w (a skirt
    // vertex takes the surface's: w is linear in the height), mapped from [fog_start, fog_end] to
    // [0, 1] the way the perspective of gl-js's u_fog_matrix maps its z.
    const float fog_w = position.w + ele_delta * drawable.matrix[2][3];
    const float fog_depth = props.fog_end * (fog_w - props.fog_start) / ((props.fog_end - props.fog_start) * fog_w);

    return {
        .position  = position,
        .uv        = uv,
        .elevation = elevation,
        .fog_depth = fog_depth,
    };
}

half4 fragment fragmentMain(FragmentStage in [[stage_in]],
                            device const TerrainEvaluatedPropsUBO& props [[buffer(idTerrainEvaluatedPropsUBO)]],
                            texture2d<float, access::sample> mapTexture [[texture(1)]],
                            sampler mapSampler [[sampler(1)]]) {
#if defined(OVERDRAW_INSPECTOR)
    return half4(1.0);
#endif

    // Sample the map texture (render-to-texture output) for the surface color.
    // The drape RTT is rendered with the core's GL-convention projection. On Vulkan
    // (NDC Y-down) that stores the RTT already flipped, so its terrain shader samples
    // 1.0 - uv.y. Metal's NDC is Y-up, so the RTT is stored upright and the V must NOT
    // be flipped here - sampling 1.0 - uv.y flips the draped map vertically (north/south
    // swapped about the view). Sample uv.y directly on Metal.
    const half4 surface_color = half4(mapTexture.sample(mapSampler, float2(in.uv.x, in.uv.y)));

    // maplibre-gl-js 6.16.0 terrain.fragment.glsl: fade the distance into the fog colour, which
    // turns into the horizon colour near the far plane. Applied to the drape sample, so the drape
    // targets do not depend on the fog.
    if (props.fog_ground_blend_opacity > 0.0 && in.fog_depth > props.fog_ground_blend) {
        const float gamma = 2.2;
        const float4 surface_color_linear = pow(float4(surface_color), float4(gamma));
        // gl-js divides by 1 - horizon-fog-blend; kept off 0 so a blend of 1 cannot divide by zero.
        const float blend_color = smoothstep(0.0, 1.0, max((in.fog_depth - props.horizon_fog_blend) / max(1.0 - props.horizon_fog_blend, 1e-6), 0.0));
        const float4 fog_horizon_color_linear = mix(pow(props.fog_color, float4(gamma)), pow(props.horizon_color, float4(gamma)), blend_color);
        // gl-js clips at the fog's far plane (fog depth 1); native draws terrain past it, so there the
        // factor stops at 1, fully fogged, instead of overshooting the fog colour. In front of the far
        // plane the fog depth is above fog-ground-blend in this branch, so this is gl-js's
        // max(depth - blend, 0) / (1 - blend).
        const float factor_fog = min((in.fog_depth - props.fog_ground_blend) / (1.0 - props.fog_ground_blend), 1.0);
        return half4(pow(mix(surface_color_linear, fog_horizon_color_linear, pow(factor_fog, 2.0) * props.fog_ground_blend_opacity), float4(1.0 / gamma)));
    }
    return surface_color;
}
)";
};

} // namespace shaders
} // namespace mln
