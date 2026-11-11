#include <mln/test/util.hpp>

#include <mln/map/mode.hpp>
#include <mln/renderer/terrain_cache_policy.hpp>

using namespace mln;

// A drape target re-bakes when its zoom key changes. Quality and Balanced key on quarter zoom steps,
// so a line whose width is interpolated over zoom keeps up during a pinch (at most 4 re-bakes per
// zoom level); Performance keys on whole zooms and re-bakes only when the tile zoom changes.

TEST(RenderTargetDrapeKey, QuarterStepInBalanced) {
    const double step = terrainLoadBudget(TerrainLoadMode::Balanced).drapeZoomStep;
    EXPECT_DOUBLE_EQ(12.25, terrain::drapeZoomKey(12.30, step));
    EXPECT_DOUBLE_EQ(12.50, terrain::drapeZoomKey(12.60, step));
}

TEST(RenderTargetDrapeKey, QuarterStepInQuality) {
    const double step = terrainLoadBudget(TerrainLoadMode::Quality).drapeZoomStep;
    EXPECT_DOUBLE_EQ(12.25, terrain::drapeZoomKey(12.30, step));
    EXPECT_DOUBLE_EQ(12.50, terrain::drapeZoomKey(12.60, step));
}

TEST(RenderTargetDrapeKey, WholeStepInPerformance) {
    const double step = terrainLoadBudget(TerrainLoadMode::Performance).drapeZoomStep;
    EXPECT_DOUBLE_EQ(12.0, terrain::drapeZoomKey(12.30, step));
    EXPECT_DOUBLE_EQ(12.0, terrain::drapeZoomKey(12.60, step));
}

TEST(RenderTargetDrapeKey, StepBoundariesBelongToTheStepAbove) {
    const double step = terrainLoadBudget(TerrainLoadMode::Balanced).drapeZoomStep;
    EXPECT_DOUBLE_EQ(12.0, terrain::drapeZoomKey(12.0, step));
    EXPECT_DOUBLE_EQ(12.75, terrain::drapeZoomKey(12.75, step));
    EXPECT_DOUBLE_EQ(12.75, terrain::drapeZoomKey(12.999, step));
    EXPECT_DOUBLE_EQ(13.0, terrain::drapeZoomKey(13.0, step));
}
