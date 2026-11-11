#pragma once

#include <mln/shaders/shader_source.hpp>
#include <mln/shaders/sky_layer_ubo.hpp>
#include <mln/shaders/mtl/shader_program.hpp>

namespace mln {
namespace shaders {

constexpr auto skyShaderPrelude = R"(

enum {
    idSkyPropsUBO = drawableReservedUBOCount,
    skyUBOCount
};

struct alignas(16) SkyPropsUBO {
    /*  0 */ float4 sky_color;
    /* 16 */ float4 horizon_color;
    /* 32 */ float2 horizon;
    /* 40 */ float2 horizon_along;
    /* 48 */ float2 horizon_up;
    /* 56 */ float sky_extent;
    /* 60 */ float sky_horizon_blend;
    /* 64 */
};
static_assert(sizeof(SkyPropsUBO) == 4 * 16, "wrong size");

)";

template <>
struct ShaderSource<BuiltIn::SkyShader, gfx::Backend::Type::Metal> {
    static constexpr auto name = "SkyShader";
    static constexpr auto vertexMainFunction = "vertexMain";
    static constexpr auto fragmentMainFunction = "fragmentMain";

    static const std::array<AttributeInfo, 1> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 0> textures;

    static constexpr auto prelude = skyShaderPrelude;
    static constexpr auto source = R"(

struct VertexStage {
    short2 pos [[attribute(0)]];
};

struct FragmentStage {
    float4 position [[position, invariant]];
    float blend;
};

FragmentStage vertex vertexMain(thread const VertexStage vertx [[stage_in]],
                                device const SkyPropsUBO& props [[buffer(idSkyPropsUBO)]]) {
    const float2 pos = float2(vertx.pos);
    return {
        .position = float4(props.horizon + (2.0 * pos.x - 1.0) * props.horizon_along + pos.y * props.horizon_up, 0.0, 1.0),
        .blend = pos.y * props.sky_extent,
    };
}

half4 fragment fragmentMain(FragmentStage in [[stage_in]],
                            device const SkyPropsUBO& props [[buffer(idSkyPropsUBO)]]) {
#if defined(OVERDRAW_INSPECTOR)
    return half4(1.0);
#endif

    if (in.blend < props.sky_horizon_blend) {
        return half4(mix(props.sky_color, props.horizon_color, pow(1.0 - in.blend / props.sky_horizon_blend, 2.0)));
    }
    return half4(props.sky_color);
}
)";
};

} // namespace shaders
} // namespace mln
