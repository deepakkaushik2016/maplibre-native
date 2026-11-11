// Generated code, do not modify this file!
#pragma once
#include <mln/shaders/shader_source.hpp>

namespace mln {
namespace shaders {

template <>
struct ShaderSource<BuiltIn::TerrainShader, gfx::Backend::Type::OpenGL> {
    static constexpr const char* name = "TerrainShader";
    static constexpr const char* vertex = R"(out vec2 v_uv;
out float v_elevation;
out highp float v_fog_depth;

layout (std140) uniform TerrainDrawableUBO {
    highp mat4 u_matrix;
    highp vec4 u_dem_tile_coords;
};

layout (std140) uniform TerrainEvaluatedPropsUBO {
    highp vec4 u_unpack;
    highp float u_exaggeration;
    highp float u_elevation_offset;
    highp float u_fog_ground_blend;
    highp float u_fog_ground_blend_opacity;
    highp vec4 u_fog_color;
    highp vec4 u_horizon_color;
    highp float u_horizon_fog_blend;
    highp float u_fog_start;
    highp float u_fog_end;
    lowp float props_pad1;
};

layout (location = 0) in vec4 a_pos; // xy = tile position, z = skirt flag (1 = skirt vertex)

uniform highp sampler2D u_dem;

void main() {
    // The mesh was generated with coordinates from 0 to EXTENT (8192)
    vec2 pos = a_pos.xy;
    v_uv = pos / 8192.0;

    // Decode the DEM and interpolate in meters via the shared prelude helper
    // (the packed Terrain-RGB/Terrarium DEM cannot be hardware-filtered, so it is
    // sampled NEAREST and interpolated after decoding, matching maplibre-gl-js and
    // the elevated layers). Map into the bound DEM tile; an ancestor tile is bound
    // as a fallback while this tile's own DEM loads. dem_coords.w = DEM dimension.
    v_elevation = get_elevation(pos, u_dem, u_dem_tile_coords, u_unpack,
                                u_dem_tile_coords.w, u_exaggeration, 1.0);

    // Skirt vertices hang below the surface by u_elevation_offset, forming a
    // curtain that hides the cracks between neighbouring tiles at different zoom
    // levels (maplibre-gl-js u_ele_delta).
    float ele_delta = a_pos.z == 1.0 ? u_elevation_offset : 0.0;
    gl_Position = u_matrix * vec4(pos.x, pos.y, v_elevation - ele_delta, 1.0);

    // maplibre-gl-js v_fog_depth: the depth of the surface point from the camera, its clip w (a skirt
    // vertex takes the surface's: w is linear in the height), mapped from [u_fog_start, u_fog_end]
    // to [0, 1] the way the perspective of gl-js's u_fog_matrix maps its z.
    highp float fog_w = gl_Position.w + ele_delta * u_matrix[2][3];
    v_fog_depth = u_fog_end * (fog_w - u_fog_start) / ((u_fog_end - u_fog_start) * fog_w);
}
)";
    static constexpr const char* fragment = R"(layout (std140) uniform TerrainEvaluatedPropsUBO {
    highp vec4 u_unpack;
    highp float u_exaggeration;
    highp float u_elevation_offset;
    highp float u_fog_ground_blend;
    highp float u_fog_ground_blend_opacity;
    highp vec4 u_fog_color;
    highp vec4 u_horizon_color;
    highp float u_horizon_fog_blend;
    highp float u_fog_start;
    highp float u_fog_end;
    lowp float props_pad1;
};

in vec2 v_uv;
in float v_elevation;
in highp float v_fog_depth;

uniform sampler2D u_map;

const highp float gamma = 2.2;

highp vec4 gammaToLinear(highp vec4 color) {
    return pow(color, vec4(gamma));
}

highp vec4 linearToGamma(highp vec4 color) {
    return pow(color, vec4(1.0 / gamma));
}

void main() {
    // Sample the map texture (render-to-texture output) for the surface color
    // Note: Y-coordinate is flipped (1.0 - y) to match OpenGL convention
    highp vec4 surface_color = texture(u_map, vec2(v_uv.x, 1.0 - v_uv.y));

    // maplibre-gl-js 6.16.0 terrain.fragment.glsl: fade the distance into the fog colour, which
    // turns into the horizon colour near the far plane. Applied to the drape sample, so the drape
    // targets do not depend on the fog.
    if (u_fog_ground_blend_opacity > 0.0 && v_fog_depth > u_fog_ground_blend) {
        highp vec4 surface_color_linear = gammaToLinear(surface_color);
        highp float blend_color = smoothstep(0.0, 1.0, max((v_fog_depth - u_horizon_fog_blend) / (1.0 - u_horizon_fog_blend), 0.0));
        highp vec4 fog_horizon_color_linear = mix(gammaToLinear(u_fog_color), gammaToLinear(u_horizon_color), blend_color);
        // gl-js clips at the fog's far plane (fog depth 1); native draws terrain past it, so there the
        // factor stops at 1, fully fogged, instead of overshooting the fog colour. In front of the far
        // plane the fog depth is above fog-ground-blend in this branch, so this is gl-js's
        // max(depth - blend, 0) / (1 - blend).
        highp float factor_fog = min((v_fog_depth - u_fog_ground_blend) / (1.0 - u_fog_ground_blend), 1.0);
        fragColor = linearToGamma(mix(surface_color_linear, fog_horizon_color_linear, pow(factor_fog, 2.0) * u_fog_ground_blend_opacity));
    } else {
        fragColor = surface_color;
    }

#if defined(OVERDRAW_INSPECTOR)
    fragColor = vec4(1.0);
#endif
}
)";
};

} // namespace shaders
} // namespace mln
