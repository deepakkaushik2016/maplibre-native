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
