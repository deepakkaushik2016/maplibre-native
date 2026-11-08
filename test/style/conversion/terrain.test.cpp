#include <mln/test/util.hpp>

#include <mln/style/conversion/json.hpp>
#include <mln/style/conversion/terrain.hpp>
#include <mln/style/conversion_impl.hpp>
#include <mln/style/rapidjson_conversion.hpp>
#include <mln/util/rapidjson.hpp>

using namespace mln;
using namespace mln::style;
using namespace mln::style::conversion;

TEST(StyleConversion, Terrain) {
    Error error;

    auto terrain = convertJSON<Terrain>(R"({"source":"dem"})", error);
    ASSERT_TRUE(terrain);
    EXPECT_EQ("dem", terrain->getSource());
    EXPECT_EQ(1.0f, terrain->getExaggeration());

    terrain = convertJSON<Terrain>(R"({"source":"dem","exaggeration":0})", error);
    ASSERT_TRUE(terrain);
    EXPECT_EQ(0.0f, terrain->getExaggeration());

    terrain = convertJSON<Terrain>(R"({"source":"dem","exaggeration":1.5})", error);
    ASSERT_TRUE(terrain);
    EXPECT_EQ(1.5f, terrain->getExaggeration());
}

// The style spec's range is at least 0. A float can't hold 1e39, so it would
// store infinity.
TEST(StyleConversion, TerrainExaggerationOutOfRange) {
    for (const auto* json : {R"({"source":"dem","exaggeration":-1})", R"({"source":"dem","exaggeration":1e39})"}) {
        SCOPED_TRACE(json);
        Error error;
        EXPECT_FALSE(convertJSON<Terrain>(json, error));
        EXPECT_EQ("terrain exaggeration must be a finite number of at least 0", error.message);
    }
}
