#include <mln/test/util.hpp>

#include <mln/geometry/dem_data.hpp>
#include <mln/layout/symbol_projection.hpp>
#include <mln/map/transform_state.hpp>
#include <mln/math/angles.hpp>
#include <mln/renderer/paint_parameters.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/image.hpp>
#include <mln/util/tile_coordinate.hpp>

#include <cmath>
#include <memory>

using namespace mln;

namespace {

constexpr double plateauMeters = 1000.0;

// A DEM tile all at `meters`, Mapbox Terrain-RGB: -10000 + (R * 65536 + G * 256 + B) * 0.1.
std::shared_ptr<const DEMData> plateauDEM(double meters) {
    const auto value = static_cast<uint32_t>(std::lround((meters + 10000.0) * 10.0));
    PremultipliedImage image({256, 256});
    for (uint32_t i = 0; i < 256 * 256; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        px[0] = static_cast<uint8_t>(value >> 16);
        px[1] = static_cast<uint8_t>((value >> 8) & 0xff);
        px[2] = static_cast<uint8_t>(value & 0xff);
        px[3] = 255;
    }
    return std::make_shared<const DEMData>(image, Tileset::RasterEncoding::Mapbox);
}

// A DEM tile rising eastwards: `slope` metres per DEM pixel, from 0 m at its western edge.
std::shared_ptr<const DEMData> eastwardSlopeDEM(double slope) {
    PremultipliedImage image({256, 256});
    for (uint32_t i = 0; i < 256 * 256; ++i) {
        const double meters = slope * ((i % 256) + 0.5);
        const auto value = static_cast<uint32_t>(std::lround((meters + 10000.0) * 10.0));
        uint8_t* px = image.data.get() + i * 4;
        px[0] = static_cast<uint8_t>(value >> 16);
        px[1] = static_cast<uint8_t>((value >> 8) & 0xff);
        px[2] = static_cast<uint8_t>(value & 0xff);
        px[3] = 255;
    }
    return std::make_shared<const DEMData>(image, Tileset::RasterEncoding::Mapbox);
}

} // namespace

// A line label of one glyph, on a 1000 m plateau at pitch 60, aligned with the viewport: its glyph
// is projected on the CPU (unlike a point symbol's, which the shader lifts by the terrain), so it
// must be placed where a point symbol at the same spot is drawn, on the plateau, and not at the
// spot's sea-level projection.
TEST(SymbolProjection, LineGlyphUsesTerrainHeight) {
    TransformState state;
    state.setSize({512, 512});
    const LatLng spot{46.0, 10.0};
    state.setLatLngZoom(spot, 13.0);
    state.setPitch(util::deg2rad(60.0));

    const TileCoordinate coordinate = TileCoordinate::fromLatLng(13.0, spot);
    const auto tileX = static_cast<uint32_t>(std::floor(coordinate.p.x));
    const auto tileY = static_cast<uint32_t>(std::floor(coordinate.p.y));
    const UnwrappedTileID tileID{13, static_cast<int32_t>(tileX), static_cast<int32_t>(tileY)};
    const Point<float> anchor{static_cast<float>((coordinate.p.x - tileX) * util::EXTENT),
                              static_cast<float>((coordinate.p.y - tileY) * util::EXTENT)};

    mat4 posMatrix;
    state.matrixFor(posMatrix, tileID);
    matrix::multiply(posMatrix, TransformParameters(state).projMatrix, posMatrix);

    // One glyph at the anchor, on a straight piece of line through it.
    PlacedSymbol symbol(anchor,
                        0,
                        16.0f,
                        16.0f,
                        {{0.0f, 0.0f}},
                        WritingModeType::Horizontal,
                        GeometryCoordinates{{static_cast<int16_t>(anchor.x - 400), static_cast<int16_t>(anchor.y)},
                                            {static_cast<int16_t>(anchor.x + 400), static_cast<int16_t>(anchor.y)}},
                        {400.0f, 400.0f},
                        std::nullopt);
    symbol.glyphOffsets = {0.0f};
    const ConstantSymbolSizeBinder sizeBinder(13.0f, 16.0f, 16.0f);

    const TileElevation plateau(plateauDEM(plateauMeters), 1.0f, 0.0f, 0.0f, 1.0f);
    ASSERT_NEAR(plateau(anchor), plateauMeters, 0.1);

    const bool pitchWithMap = false;
    const bool rotateWithMap = true;
    const float pixelsToTileUnits = tileID.pixelsToTileUnits(1.0f, 13.0f);
    const mat4 labelPlaneMatrix = getLabelPlaneMatrix(posMatrix, pitchWithMap, rotateWithMap, state, pixelsToTileUnits);
    // Where a point symbol at the spot is drawn (symbol_sdf.vertex: the label-plane matrix applied
    // to the anchor lifted by the terrain under it), and where it would sit at sea level.
    const auto viewportPoint = [&](double meters) {
        vec4 p = {{anchor.x, anchor.y, meters, 1.0}};
        matrix::transformMat4(p, p, labelPlaneMatrix);
        return Point<double>{p[0] / p[3], p[1] / p[3]};
    };
    const Point<double> onPlateau = viewportPoint(plateauMeters);
    const Point<double> atSeaLevel = viewportPoint(0.0);
    ASSERT_GT(std::hypot(onPlateau.x - atSeaLevel.x, onPlateau.y - atSeaLevel.y), 50.0)
        << "the plateau does not move the spot on screen, so the test cannot tell the two apart";

    SymbolBucket::DynamicAttributeVector dynamicVertices;
    reprojectLineLabels(dynamicVertices,
                        {symbol},
                        posMatrix,
                        pitchWithMap,
                        rotateWithMap,
                        /*keepUpright*/ true,
                        tileID,
                        sizeBinder,
                        state,
                        &plateau);
    ASSERT_FALSE(dynamicVertices.empty());
    const auto& glyph = dynamicVertices.at(0).a1;
    EXPECT_NEAR(glyph[0], onPlateau.x, 0.01) << "sea level would put it at x " << atSeaLevel.x;
    EXPECT_NEAR(glyph[1], onPlateau.y, 0.01) << "sea level would put it at y " << atSeaLevel.y;

    // Without terrain the glyph stays at the sea-level projection.
    SymbolBucket::DynamicAttributeVector flatVertices;
    reprojectLineLabels(flatVertices,
                        {symbol},
                        posMatrix,
                        pitchWithMap,
                        rotateWithMap,
                        /*keepUpright*/ true,
                        tileID,
                        sizeBinder,
                        state,
                        nullptr);
    ASSERT_FALSE(flatVertices.empty());
    EXPECT_NEAR(flatVertices.at(0).a1[0], atSeaLevel.x, 0.01);
    EXPECT_NEAR(flatVertices.at(0).a1[1], atSeaLevel.y, 0.01);
}

// A line label of five glyphs aligned with the viewport, along an east-west line of vertices every
// 250 tile units, over ground that rises 2000 m across the tile. Each glyph must sit on the line as
// projected with the terrain height under each vertex (maplibre-gl-js `updateLineLabels`: the
// glyph's distance from the anchor walked along those projected vertices), not on the line lifted
// by the anchor's height alone, nor at sea level.
TEST(SymbolProjection, LineGlyphsFollowTheTerrainUnderEachVertex) {
    TransformState state;
    state.setSize({512, 512});
    const LatLng spot{46.0, 10.0};
    state.setLatLngZoom(spot, 13.0);
    state.setPitch(util::deg2rad(60.0));

    const TileCoordinate coordinate = TileCoordinate::fromLatLng(13.0, spot);
    const auto tileX = static_cast<uint32_t>(std::floor(coordinate.p.x));
    const auto tileY = static_cast<uint32_t>(std::floor(coordinate.p.y));
    const UnwrappedTileID tileID{13, static_cast<int32_t>(tileX), static_cast<int32_t>(tileY)};
    // The anchor on a vertex-free stretch: vertices every 250 units, the anchor 100 past one.
    const auto anchorY = static_cast<int16_t>((coordinate.p.y - tileY) * util::EXTENT);
    GeometryCoordinates line;
    for (int16_t x = 3000; x <= 6000; x += 250) {
        line.emplace_back(x, anchorY);
    }
    const Point<float> anchor{4600.0f, static_cast<float>(anchorY)};
    const std::size_t segment = 6; // between x = 4500 and 4750
    std::vector<float> tileDistances(line.size());
    for (std::size_t i = 0; i < line.size(); ++i) {
        tileDistances[i] = std::abs(static_cast<float>(line[i].x) - anchor.x);
    }

    mat4 posMatrix;
    state.matrixFor(posMatrix, tileID);
    matrix::multiply(posMatrix, TransformParameters(state).projMatrix, posMatrix);

    constexpr float fontSize = 24.0f;
    PlacedSymbol symbol(anchor,
                        segment,
                        fontSize,
                        fontSize,
                        {{0.0f, 0.0f}},
                        WritingModeType::Horizontal,
                        line,
                        tileDistances,
                        std::nullopt);
    // Glyph offsets in ems of 24 px (util::ONE_EM): -60 to 60 px from the anchor, before the
    // perspective scaling, so the outer glyphs reach past the anchor's neighbouring vertices.
    symbol.glyphOffsets = {-60.0f, -30.0f, 0.0f, 30.0f, 60.0f};
    const ConstantSymbolSizeBinder sizeBinder(13.0f, fontSize, fontSize);

    const TileElevation slope(eastwardSlopeDEM(2000.0 / 256.0), 1.0f, 0.0f, 0.0f, 1.0f);
    ASSERT_NEAR(slope({3000.0f, anchor.y}) - slope({6000.0f, anchor.y}), -3000.0 * 2000.0 / util::EXTENT, 0.5);

    const bool pitchWithMap = false;
    const bool rotateWithMap = true;
    const mat4 labelPlaneMatrix = getLabelPlaneMatrix(
        posMatrix, pitchWithMap, rotateWithMap, state, tileID.pixelsToTileUnits(1.0f, 13.0f));
    const auto toScreen = [&](const Point<float>& p, double meters) {
        vec4 v = {{p.x, p.y, meters, 1.0}};
        matrix::transformMat4(v, v, labelPlaneMatrix);
        return Point<double>{v[0] / v[3], v[1] / v[3]};
    };

    // The expected glyph positions: each glyph `offset` px from the anchor, walked along the line's
    // vertices projected at `heightOf` them.
    const auto expectedGlyphs = [&](const auto& heightOf) {
        vec4 a = {{anchor.x, anchor.y, heightOf(anchor), 1.0}};
        matrix::transformMat4(a, a, posMatrix);
        const double perspectiveRatio = 0.5 + 0.5 * (a[3] / state.getCameraToCenterDistance());
        const double fontScale = fontSize / perspectiveRatio / util::ONE_EM;
        std::vector<Point<double>> glyphs;
        for (const float offset : symbol.glyphOffsets) {
            const double distance = std::abs(offset) * fontScale;
            const int dir = offset > 0 ? 1 : -1;
            Point<double> prev = toScreen(anchor, heightOf(anchor));
            double walked = 0.0;
            for (int i = dir > 0 ? static_cast<int>(segment) + 1 : static_cast<int>(segment);; i += dir) {
                const Point<float> vertex{static_cast<float>(line.at(i).x), static_cast<float>(line.at(i).y)};
                const Point<double> next = toScreen(vertex, heightOf(vertex));
                const double length = std::hypot(next.x - prev.x, next.y - prev.y);
                if (walked + length > distance) {
                    const double t = (distance - walked) / length;
                    glyphs.push_back({prev.x + (next.x - prev.x) * t, prev.y + (next.y - prev.y) * t});
                    break;
                }
                walked += length;
                prev = next;
            }
        }
        return glyphs;
    };
    const auto perVertex = expectedGlyphs([&](const Point<float>& p) { return static_cast<double>(slope(p)); });
    const auto anchorOnly = expectedGlyphs([&](const Point<float>&) { return static_cast<double>(slope(anchor)); });
    const auto seaLevel = expectedGlyphs([](const Point<float>&) { return 0.0; });
    // The outer glyphs lie past the anchor's segment, where the per-vertex line bends away from
    // the anchor-height one by more than a pixel; otherwise the test could not tell them apart.
    ASSERT_GT(std::hypot(perVertex.front().x - anchorOnly.front().x, perVertex.front().y - anchorOnly.front().y), 1.0);
    ASSERT_GT(std::hypot(perVertex.back().x - anchorOnly.back().x, perVertex.back().y - anchorOnly.back().y), 1.0);

    SymbolBucket::DynamicAttributeVector dynamicVertices;
    reprojectLineLabels(dynamicVertices,
                        {symbol},
                        posMatrix,
                        pitchWithMap,
                        rotateWithMap,
                        /*keepUpright*/ false,
                        tileID,
                        sizeBinder,
                        state,
                        &slope);
    const std::size_t glyphs = symbol.glyphOffsets.size();
    ASSERT_EQ(dynamicVertices.elements() % glyphs, 0u);
    const std::size_t perGlyph = dynamicVertices.elements() / glyphs;
    for (std::size_t g = 0; g < glyphs; ++g) {
        SCOPED_TRACE(::testing::Message()
                     << "glyph " << g << ": anchor height alone would put it at (" << anchorOnly[g].x << ", "
                     << anchorOnly[g].y << "), sea level at (" << seaLevel[g].x << ", " << seaLevel[g].y << ")");
        const auto& placed = dynamicVertices.at(g * perGlyph).a1;
        EXPECT_NEAR(placed[0], perVertex[g].x, 0.05);
        EXPECT_NEAR(placed[1], perVertex[g].y, 0.05);
    }
}
