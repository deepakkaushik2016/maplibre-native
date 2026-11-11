layout (std140) uniform SkyPropsUBO {
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
