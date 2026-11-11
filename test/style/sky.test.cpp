#include <mln/test/util.hpp>
#include <mln/test/fixture_log_observer.hpp>
#include <mln/test/stub_file_source.hpp>

#include <mln/renderer/property_evaluation_parameters.hpp>
#include <mln/renderer/transition_parameters.hpp>
#include <mln/style/parser.hpp>
#include <mln/style/sky.hpp>
#include <mln/style/sky_impl.hpp>
#include <mln/style/style_impl.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/run_loop.hpp>

#include <string>

using namespace mln;
using namespace mln::style;
using namespace std::literals::chrono_literals;

namespace {

std::string styleWithSky(const std::string& sky) {
    return R"({"version":8,"sources":{},"layers":[],"sky":)" + sky + "}";
}

// What the renderer will read: the sky's properties transitioned from a prior state at
// `start`, then evaluated at zoom `zoom` and time `now`.
SkyProperties::PossiblyEvaluated evaluate(
    const Sky::Impl& impl, SkyProperties::Unevaluated prior, TimePoint start, TimePoint now, float zoom = 0) {
    const auto transitioning = impl.properties.transitioned(TransitionParameters{start, TransitionOptions{}},
                                                            std::move(prior));
    ZoomHistory zoomHistory;
    zoomHistory.update(zoom, now);
    return transitioning.evaluate(PropertyEvaluationParameters{zoomHistory, now, Duration::zero()});
}

SkyProperties::PossiblyEvaluated evaluate(const Sky::Impl& impl, float zoom = 0) {
    return evaluate(impl, impl.properties.untransitioned(), TimePoint::min(), TimePoint::min(), zoom);
}

} // namespace

TEST(Sky, ParsesAllProperties) {
    FixtureLog log;
    Parser parser;
    ASSERT_FALSE(parser.parse(styleWithSky(R"({
        "sky-color": "#199EF3",
        "horizon-color": "#00ff00",
        "fog-color": "#0000ff",
        "fog-ground-blend": 0.1,
        "horizon-fog-blend": 0.2,
        "sky-horizon-blend": 0.3,
        "atmosphere-blend": ["interpolate", ["linear"], ["zoom"], 0, 1, 10, 1, 12, 0],
        "sky-color-transition": {"duration": 500, "delay": 100}
    })")));
    EXPECT_TRUE(log.empty());
    ASSERT_TRUE(parser.sky);
    const Sky& sky = *parser.sky;

    EXPECT_EQ(*Color::parse("#199EF3"), sky.getSkyColor().asConstant());
    EXPECT_EQ(Color::green(), sky.getHorizonColor().asConstant());
    EXPECT_EQ(Color::blue(), sky.getFogColor().asConstant());
    EXPECT_FLOAT_EQ(0.1f, sky.getFogGroundBlend().asConstant());
    EXPECT_FLOAT_EQ(0.2f, sky.getHorizonFogBlend().asConstant());
    EXPECT_FLOAT_EQ(0.3f, sky.getSkyHorizonBlend().asConstant());
    ASSERT_TRUE(sky.getAtmosphereBlend().isExpression());
    EXPECT_EQ(std::chrono::milliseconds(500), sky.getSkyColorTransition().duration);
    EXPECT_EQ(std::chrono::milliseconds(100), sky.getSkyColorTransition().delay);

    // The zoom curve is carried through to evaluation.
    EXPECT_FLOAT_EQ(1.0f, evaluate(*sky.impl, 5).get<SkyAtmosphereBlend>());
    EXPECT_FLOAT_EQ(0.5f, evaluate(*sky.impl, 11).get<SkyAtmosphereBlend>());
    EXPECT_FLOAT_EQ(0.0f, evaluate(*sky.impl, 14).get<SkyAtmosphereBlend>());

    // Every property is reachable by its style-spec name too (the path JSON get/set takes).
    for (const char* name : {"sky-color",
                             "horizon-color",
                             "fog-color",
                             "fog-ground-blend",
                             "horizon-fog-blend",
                             "sky-horizon-blend",
                             "atmosphere-blend"}) {
        SCOPED_TRACE(name);
        EXPECT_NE(StyleProperty::Kind::Undefined, sky.getProperty(name).getKind());
    }
    EXPECT_FLOAT_EQ(0.1f, sky.getProperty("fog-ground-blend").getValue().get<double>());
    EXPECT_EQ(StyleProperty::Kind::Transition, sky.getProperty("sky-color-transition").getKind());
}

TEST(Sky, DefaultsMatchSpec) {
    Parser parser;
    ASSERT_FALSE(parser.parse(styleWithSky("{}")));
    ASSERT_TRUE(parser.sky);

    EXPECT_EQ(*Color::parse("#88C6FC"), Sky::getDefaultSkyColor());
    EXPECT_EQ(Color::white(), Sky::getDefaultHorizonColor());
    EXPECT_EQ(Color::white(), Sky::getDefaultFogColor());
    EXPECT_FLOAT_EQ(0.5f, Sky::getDefaultFogGroundBlend());
    EXPECT_FLOAT_EQ(0.8f, Sky::getDefaultHorizonFogBlend());
    EXPECT_FLOAT_EQ(0.8f, Sky::getDefaultSkyHorizonBlend());
    EXPECT_FLOAT_EQ(0.8f, Sky::getDefaultAtmosphereBlend());

    const auto evaluated = evaluate(*parser.sky->impl);
    EXPECT_EQ(*Color::parse("#88C6FC"), evaluated.get<SkySkyColor>());
    EXPECT_EQ(Color::white(), evaluated.get<SkyHorizonColor>());
    EXPECT_EQ(Color::white(), evaluated.get<SkyFogColor>());
    EXPECT_FLOAT_EQ(0.5f, evaluated.get<SkyFogGroundBlend>());
    EXPECT_FLOAT_EQ(0.8f, evaluated.get<SkyHorizonFogBlend>());
    EXPECT_FLOAT_EQ(0.8f, evaluated.get<SkySkyHorizonBlend>());
    EXPECT_FLOAT_EQ(0.8f, evaluated.get<SkyAtmosphereBlend>());
}

TEST(Sky, AbsentMeansNull) {
    util::RunLoop loop;
    Style::Impl style{std::make_shared<StubFileSource>(), 1.0, {Scheduler::GetBackground(), {}}};

    style.loadJSON(R"({"version":8,"sources":{},"layers":[]})");
    EXPECT_EQ(nullptr, style.getSky());

    // A style with a sky, then one without: the second does not inherit the first's sky.
    style.loadJSON(styleWithSky(R"({"sky-color":"red"})"));
    ASSERT_NE(nullptr, style.getSky());
    EXPECT_EQ(Color::red(), style.getSky()->getSkyColor().asConstant());

    style.loadJSON(R"({"version":8,"sources":{},"layers":[]})");
    EXPECT_EQ(nullptr, style.getSky());

    // Set and cleared at runtime.
    style.setSky(std::make_unique<Sky>());
    ASSERT_NE(nullptr, style.getSky());
    style.setSky(nullptr);
    EXPECT_EQ(nullptr, style.getSky());
}

TEST(Sky, TransitionsInterpolate) {
    Sky sky;
    sky.setSkyColor(Color::red());
    const auto red = sky.impl->properties.untransitioned();

    sky.setSkyColor(Color::blue());
    sky.setSkyColorTransition(TransitionOptions{{300ms}});

    const TimePoint start = TimePoint::min();
    const auto atStart = evaluate(*sky.impl, red, start, start).get<SkySkyColor>();
    EXPECT_EQ(Color::red(), atStart);

    const auto midway = evaluate(*sky.impl, red, start, start + 150ms).get<SkySkyColor>();
    // Halfway in time is the default ease's value at 0.5: purple, mostly blue.
    const auto eased = static_cast<float>(util::DEFAULT_TRANSITION_EASE.solve(0.5, 0.001));
    EXPECT_NEAR(1.0f - eased, midway.r, 1e-4);
    EXPECT_NEAR(0.0f, midway.g, 1e-6);
    EXPECT_NEAR(eased, midway.b, 1e-4);
    EXPECT_GT(midway.r, 0.0f);
    EXPECT_GT(midway.b, midway.r);

    const auto atEnd = evaluate(*sky.impl, red, start, start + 300ms).get<SkySkyColor>();
    EXPECT_EQ(Color::blue(), atEnd);
}

TEST(Sky, InvalidValueWarnsAndKeepsDefault) {
    FixtureLog log;
    Parser parser;
    ASSERT_FALSE(parser.parse(styleWithSky(R"({"fog-ground-blend":"high","sky-color":"red"})")));
    ASSERT_TRUE(parser.sky);

    EXPECT_EQ(1u, log.count({EventSeverity::Warning, Event::ParseStyle, -1, "fog-ground-blend"}, true));
    EXPECT_EQ(0u, log.uncheckedCount());

    // The bad property keeps its default; the good one still applies.
    EXPECT_TRUE(parser.sky->getFogGroundBlend().isUndefined());
    EXPECT_FLOAT_EQ(0.5f, evaluate(*parser.sky->impl).get<SkyFogGroundBlend>());
    EXPECT_EQ(Color::red(), parser.sky->getSkyColor().asConstant());
}
