#pragma once

#include <mln/shaders/layer_ubo.hpp>

namespace mln {
namespace shaders {

/// The sky quad (RenderSky). The quad's vertices are (0|1, 0|1): x runs along the horizon line,
/// y up from it, so a vertex lands at horizon + (2x - 1) * horizon_along + y * horizon_up in clip
/// space, and is sky_extent pixels above the horizon line when y is 1.
struct alignas(16) SkyPropsUBO {
    /*  0 */ Color sky_color;
    /* 16 */ Color horizon_color;
    /* 32 */ std::array<float, 2> horizon;
    /* 40 */ std::array<float, 2> horizon_along;
    /* 48 */ std::array<float, 2> horizon_up;
    /* 56 */ float sky_extent;
    /// The height of the sky-horizon gradient, in pixels.
    /* 60 */ float sky_horizon_blend;
    /* 64 */
};
static_assert(sizeof(SkyPropsUBO) == 4 * 16);

} // namespace shaders
} // namespace mln
