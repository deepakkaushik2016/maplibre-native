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
        // gl-js divides by 1 - horizon-fog-blend; kept off 0 so a blend of 1 cannot divide by zero.
        highp float blend_color = smoothstep(0.0, 1.0, max((v_fog_depth - u_horizon_fog_blend) / max(1.0 - u_horizon_fog_blend, 1e-6), 0.0));
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
