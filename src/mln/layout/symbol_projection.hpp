#pragma once

#include <mln/util/mat4.hpp>
#include <mln/gfx/vertex_buffer.hpp>
#include <mln/renderer/buckets/symbol_bucket.hpp>
#include <mln/renderer/render_terrain.hpp>

namespace mln {

class TransformState;
class SymbolSizeBinder;
class PlacedSymbol;
namespace style {
class SymbolPropertyValues;
} // end namespace style

struct TileDistance {
    TileDistance(float prevTileDistance_, float lastSegmentViewportDistance_)
        : prevTileDistance(prevTileDistance_),
          lastSegmentViewportDistance(lastSegmentViewportDistance_) {}
    float prevTileDistance;
    float lastSegmentViewportDistance;
};

struct PlacedGlyph {
    PlacedGlyph() = default;

    PlacedGlyph(Point<float> point_, float angle_, std::optional<TileDistance> tileDistance_)
        : point(point_),
          angle(angle_),
          tileDistance(std::move(tileDistance_)) {}
    PlacedGlyph(PlacedGlyph&& other) noexcept
        : point(other.point),
          angle(other.angle),
          tileDistance(std::move(other.tileDistance)) {}
    PlacedGlyph(const PlacedGlyph& other) = default;
    Point<float> point;
    float angle;
    std::optional<TileDistance> tileDistance;
};

float evaluateSizeForFeature(const ZoomEvaluatedSize& zoomEvaluatedSize, const PlacedSymbol& placedSymbol);
mat4 getLabelPlaneMatrix(
    const mat4& posMatrix, bool pitchWithMap, bool rotateWithMap, const TransformState& state, float pixelsToTileUnits);
mat4 getGlCoordMatrix(
    const mat4& posMatrix, bool pitchWithMap, bool rotateWithMap, const TransformState& state, float pixelsToTileUnits);

/// The exaggerated terrain height under a tile's points; null where symbols sit at sea level.
using TileElevation = RenderTerrain::TileElevation;

using PointAndCameraDistance = std::pair<Point<float>, float>;
PointAndCameraDistance project(const Point<float>& point, const mat4& matrix);
/// Projects a tile point `height` metres up, as maplibre-gl-js `projectWithMatrix` with an elevation.
PointAndCameraDistance project(const Point<float>& point, const mat4& matrix, float height);

/// The terrain height under `point`, or 0 without terrain (`elevation` null).
inline float heightAt(const TileElevation* elevation, const Point<float>& point) {
    return elevation ? (*elevation)(point) : 0.0f;
}

/// Places the glyphs of `placedSymbols`' line labels for the current camera, lifted by the terrain
/// under the line (`elevation`, null for sea level), as maplibre-gl-js `updateLineLabels`.
void reprojectLineLabels(SymbolBucket::DynamicAttributeVector&,
                         const std::vector<PlacedSymbol>&,
                         const mat4& posMatrix,
                         bool pitchWithMap,
                         bool rotateWithMap,
                         bool keepUpright,
                         const UnwrappedTileID& tileID,
                         const SymbolSizeBinder& sizeBinder,
                         const TransformState&,
                         const TileElevation* elevation);

std::optional<std::pair<PlacedGlyph, PlacedGlyph>> placeFirstAndLastGlyph(float fontScale,
                                                                          float lineOffsetX,
                                                                          float lineOffsetY,
                                                                          bool flip,
                                                                          const Point<float>& anchorPoint,
                                                                          const Point<float>& tileAnchorPoint,
                                                                          const PlacedSymbol& symbol,
                                                                          const mat4& labelPlaneMatrix,
                                                                          bool returnTileDistance);

void hideGlyphs(std::size_t numGlyphs, SymbolBucket::DynamicAttributeVector& dynamicVertexArray);
void addDynamicAttributes(const Point<float>& anchorPoint,
                          float angle,
                          SymbolBucket::DynamicAttributeVector& dynamicAttributeData);

} // end namespace mln
