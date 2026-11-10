#include <mln/test/util.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/context.hpp>
#include <mln/gfx/headless_backend.hpp>
#include <mln/gfx/texture2d.hpp>
#include <mln/renderer/texture_pool.hpp>
#include <mln/tile/tile_id.hpp>

#include <memory>
#include <set>

using namespace mln;

namespace {

UnwrappedTileID z13(uint32_t x, uint32_t y) {
    return UnwrappedTileID(0, CanonicalTileID(13, x, y));
}

UnwrappedTileID z12(uint32_t x, uint32_t y) {
    return UnwrappedTileID(0, CanonicalTileID(12, x, y));
}

class TerrainDrapePool : public ::testing::Test {
protected:
    std::unique_ptr<gfx::HeadlessBackend> backend = gfx::HeadlessBackend::Create({64, 64});
    gfx::BackendScope scope{*backend->getRendererBackend()};
    gfx::Context& context = backend->getRendererBackend()->getContext();
    TexturePool pool{64};
    const Color background = Color::white();

    gfx::Texture2D* textureOf(const UnwrappedTileID& id) {
        const auto target = pool.getRenderTarget(id);
        return target ? target->getTexture().get() : nullptr;
    }
};

} // namespace

// When the cover moves on (a fly-to crossing a zoom level replaces every tile), the
// targets that leave it hand their GPU textures to the targets that join it. Making a
// target's texture and depth-stencil buffer costs about 2 ms of driver work on a phone,
// and a whole cover is about 60 targets, so building them anew stalls one frame for over
// 100 ms.
TEST_F(TerrainDrapePool, ATargetThatLeavesTheCoverLendsItsTextureToOneThatJoins) {
    EXPECT_TRUE(pool.updateRenderTargets(context, {z13(1, 1), z13(1, 2)}, background));
    auto* const leaving = textureOf(z13(1, 1));
    auto* const staying = textureOf(z13(1, 2));
    ASSERT_NE(nullptr, leaving);
    ASSERT_NE(nullptr, staying);

    EXPECT_TRUE(pool.updateRenderTargets(context, {z13(1, 2), z13(2, 2)}, background));
    EXPECT_EQ(nullptr, pool.getRenderTarget(z13(1, 1)));
    EXPECT_EQ(staying, textureOf(z13(1, 2)));
    EXPECT_EQ(leaving, textureOf(z13(2, 2)));
    // The joining target is a new one, aimed at its own tile.
    ASSERT_TRUE(pool.getRenderTarget(z13(2, 2))->getDrapeTileID());
    EXPECT_EQ(z13(2, 2), *pool.getRenderTarget(z13(2, 2))->getDrapeTileID());
}

TEST_F(TerrainDrapePool, AWholeNewCoverReusesEveryTexture) {
    std::set<UnwrappedTileID> fine;
    std::set<UnwrappedTileID> coarse;
    for (uint32_t i = 0; i < 4; ++i) {
        fine.insert(z13(i, 0));
        coarse.insert(z12(i, 0));
    }
    pool.updateRenderTargets(context, fine, background);
    std::set<gfx::Texture2D*> before;
    for (const auto& id : fine) before.insert(textureOf(id));

    EXPECT_TRUE(pool.updateRenderTargets(context, coarse, background));
    std::set<gfx::Texture2D*> after;
    for (const auto& id : coarse) after.insert(textureOf(id));
    EXPECT_EQ(before, after);
}

// A target that something else still holds keeps its texture: only the pool's own
// targets can be lent out.
TEST_F(TerrainDrapePool, AHeldTargetKeepsItsTexture) {
    pool.updateRenderTargets(context, {z13(1, 1)}, background);
    const auto held = pool.getRenderTarget(z13(1, 1));
    auto* const heldTexture = held->getTexture().get();

    pool.updateRenderTargets(context, {z13(2, 2)}, background);
    EXPECT_EQ(heldTexture, held->getTexture().get());
    EXPECT_NE(heldTexture, textureOf(z13(2, 2)));
}

// No texture is kept back for later: a cover that shrinks frees what it no longer uses,
// so memory at rest is what the cover needs, as before.
TEST_F(TerrainDrapePool, AShrinkingCoverKeepsNoSpareTexture) {
    pool.updateRenderTargets(context, {z13(1, 1), z13(1, 2)}, background);
    std::weak_ptr<gfx::Texture2D> leaving = pool.getRenderTarget(z13(1, 1))->getTexture();

    EXPECT_FALSE(pool.updateRenderTargets(context, {z13(1, 2)}, background));
    EXPECT_TRUE(leaving.expired());

    pool.updateRenderTargets(context, {}, background);
    EXPECT_EQ(nullptr, pool.getRenderTarget(z13(1, 2)));
}

TEST_F(TerrainDrapePool, AnUnchangedCoverCreatesNothing) {
    EXPECT_TRUE(pool.updateRenderTargets(context, {z13(1, 1)}, background));
    auto* const texture = textureOf(z13(1, 1));
    EXPECT_FALSE(pool.updateRenderTargets(context, {z13(1, 1)}, background));
    EXPECT_EQ(texture, textureOf(z13(1, 1)));
}
