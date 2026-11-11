#pragma once

#include <mln/style/sky.hpp>
#include <mln/style/property_value.hpp>
#include <mln/style/properties.hpp>
#include <mln/renderer/property_evaluator.hpp>
#include <mln/util/color.hpp>

namespace mln {
namespace style {

// Sky properties: zoom expressions and transitions, never data-driven (style spec v8 `sky`).
template <class T>
class SkyProperty {
public:
    using TransitionableType = Transitionable<PropertyValue<T>>;
    using UnevaluatedType = Transitioning<PropertyValue<T>>;
    using EvaluatorType = PropertyEvaluator<T>;
    using PossiblyEvaluatedType = T;
    using Type = T;
    static constexpr bool IsDataDriven = false;
    static constexpr bool IsOverridable = false;
};

struct SkySkyColor : SkyProperty<Color> {
    // Parsed once, so the default is exactly what a style's "#88C6FC" parses to.
    static Color defaultValue() {
        static const Color color = *Color::parse("#88C6FC");
        return color;
    }
};

struct SkyHorizonColor : SkyProperty<Color> {
    static Color defaultValue() { return Color::white(); }
};

struct SkyFogColor : SkyProperty<Color> {
    static Color defaultValue() { return Color::white(); }
};

struct SkyFogGroundBlend : SkyProperty<float> {
    static float defaultValue() { return 0.5f; }
};

struct SkyHorizonFogBlend : SkyProperty<float> {
    static float defaultValue() { return 0.8f; }
};

struct SkySkyHorizonBlend : SkyProperty<float> {
    static float defaultValue() { return 0.8f; }
};

// Held so the style round-trips; applies to globe only, which is not rendered.
struct SkyAtmosphereBlend : SkyProperty<float> {
    static float defaultValue() { return 0.8f; }
};

using SkyProperties = Properties<SkySkyColor,
                                 SkyHorizonColor,
                                 SkyFogColor,
                                 SkyFogGroundBlend,
                                 SkyHorizonFogBlend,
                                 SkySkyHorizonBlend,
                                 SkyAtmosphereBlend>;

class Sky::Impl {
public:
    SkyProperties::Transitionable properties;
};

} // namespace style
} // namespace mln
