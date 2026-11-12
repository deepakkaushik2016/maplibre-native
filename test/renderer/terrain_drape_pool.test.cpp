#include <mln/test/util.hpp>
#include <mln/test/map_adapter.hpp>
#include <mln/test/renderer_test_access.hpp>
#include <mln/test/stub_file_source.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/context.hpp>
#include <mln/gfx/headless_backend.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/gfx/offscreen_texture.hpp>
#include <mln/gfx/texture2d.hpp>
#include <mln/renderer/texture_pool.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/map/mode.hpp>
#include <mln/style/style.hpp>
#include <mln/tile/tile_id.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>

#include <memory>
#include <set>
#include <string>
#include <vector>

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

// The texture a leaving target drew into can still be sampled by the terrain drawable of its
// tile, which holds the texture itself (not the target). Such a texture is not lent: the new
// target makes its own, so nothing samples a texture that is cleared for another tile.
TEST_F(TerrainDrapePool, ATextureStillHeldIsNotLent) {
    pool.updateRenderTargets(context, {z13(1, 1)}, background);
    const gfx::Texture2DPtr held = pool.getRenderTarget(z13(1, 1))->getTexture();

    EXPECT_TRUE(pool.updateRenderTargets(context, {z13(2, 2)}, background));
    ASSERT_NE(nullptr, textureOf(z13(2, 2)));
    EXPECT_NE(held.get(), textureOf(z13(2, 2)));
}

namespace {
RenderTarget::Spec drapeSpec(uint32_t size) {
    return {.size = {size, size}, .type = gfx::TextureChannelDataType::UnsignedByte, .stencil = true};
}
} // namespace

// A target lends only to a target of its own size and formats.
TEST_F(TerrainDrapePool, ATargetLendsOnlyToItsOwnSizeAndFormats) {
    const RenderTarget target(context, {64, 64}, gfx::TextureChannelDataType::UnsignedByte, /*stencil=*/true);
    EXPECT_TRUE(target.canLend(drapeSpec(64)));
    EXPECT_FALSE(target.canLend(drapeSpec(32)));
    EXPECT_FALSE(target.canLend({.size = {64, 64}, .type = gfx::TextureChannelDataType::HalfFloat, .stencil = true}));
    EXPECT_FALSE(
        target.canLend({.size = {64, 64}, .type = gfx::TextureChannelDataType::UnsignedByte, .stencil = false}));
}

TEST_F(TerrainDrapePool, ATargetWhoseTextureIsHeldCannotLend) {
    const RenderTarget target(context, {64, 64}, gfx::TextureChannelDataType::UnsignedByte, /*stencil=*/true);
    const gfx::Texture2DPtr held = const_cast<RenderTarget&>(target).getTexture();
    EXPECT_FALSE(target.canLend(drapeSpec(64)));
}

// A target that has lent its texture has none, and says so instead of handing out a dangling one.
TEST_F(TerrainDrapePool, ATargetThatHasLentItsTextureHasNone) {
    RenderTarget target(context, {64, 64}, gfx::TextureChannelDataType::UnsignedByte, /*stencil=*/true);
    auto offscreen = target.takeOffscreenTexture();
    ASSERT_NE(nullptr, offscreen);
    EXPECT_EQ(nullptr, target.getTexture());
    EXPECT_FALSE(target.canLend(drapeSpec(64)));
}

// The drape target size follows the map's TerrainLoadMode: Performance drapes at 512 px
// (TerrainLoadBudget::drapeQualityFactor 1), a quarter of the memory of the 1024 px targets the
// other modes use. These render a terrain map, so they read the pool through RendererTestAccess.
namespace {

const char* const drapeModeStyle = R"STYLE({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"],
      "encoding": "mapbox",
      "maxzoom": 10,
      "tileSize": 256
    }
  },
  "terrain": {"source": "dem", "exaggeration": 1.0},
  "layers": [{"id": "background", "type": "background", "paint": {"background-color": "white"}}]
})STYLE";

// A 1000 m plateau in Terrain-RGB: -10000 + (1 * 65536 + 173 * 256 + 176) * 0.1 = 1000.
std::string plateauDEMTile() {
    PremultipliedImage image({256, 256});
    for (uint32_t i = 0; i < 256 * 256; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        px[0] = 1;
        px[1] = 173;
        px[2] = 176;
        px[3] = 255;
    }
    return encodePNG(image);
}

struct DrapeModeScene {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    std::string tile = plateauDEMTile();
    HeadlessFrontend frontend{{256, 256}, 1};
    MapAdapter map{frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())};

    explicit DrapeModeScene(const char* style = drapeModeStyle) {
        fileSource->tileResponse = [this](const Resource&) {
            Response res;
            res.data = std::make_shared<std::string>(tile);
            return res;
        };
        map.getStyle().loadJSON(style);
        map.jumpTo(CameraOptions().withCenter(LatLng{46.0, 10.0}).withZoom(12.0).withPitch(45.0));
    }

    // One settled still frame in `mode`; returns the GPU textures alive after it.
    int render(TerrainLoadMode mode) {
        map.setTerrainLoadMode(mode);
        return frontend.render(map).stats.numActiveTextures;
    }

    std::vector<mln::Size> targetSizes() { return RendererTestAccess::drapeTargetSizes(*frontend.getRenderer()); }
};

// The same terrain with a hillshade draped onto it: its drawables render into every drape target.
const char* const drapedHillshadeStyle = R"STYLE({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"],
      "encoding": "mapbox",
      "maxzoom": 10,
      "tileSize": 256
    }
  },
  "terrain": {"source": "dem", "exaggeration": 1.0},
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "white"}},
    {"id": "hillshade", "type": "hillshade", "source": "dem"}
  ]
})STYLE";

// Every drape target has this size; the pool is not empty.
::testing::AssertionResult allTargetsAre(const std::vector<mln::Size>& sizes, uint32_t size) {
    if (sizes.empty()) return ::testing::AssertionFailure() << "no drape targets";
    for (const auto& s : sizes) {
        if (s.width != size || s.height != size) {
            return ::testing::AssertionFailure()
                   << "a drape target is " << s.width << "x" << s.height << ", not " << size << "x" << size;
        }
    }
    return ::testing::AssertionSuccess();
}

} // namespace

TEST_F(TerrainDrapePool, PerformanceModeUses512Targets) {
    DrapeModeScene scene;
    scene.render(TerrainLoadMode::Performance);
    const auto sizes = scene.targetSizes();
    EXPECT_TRUE(allTargetsAre(sizes, 512));
    // Colour (RGBA8) and depth-stencil at 4 bytes a texel each, per target; Performance caps
    // the mesh, and with it the drape targets, at 24 tiles (TerrainLoadBudget::maxMeshTiles).
    size_t bytes = 0;
    for (const auto& s : sizes) bytes += size_t{s.width} * s.height * 8;
    EXPECT_LE(bytes, size_t{24} * 512 * 512 * 8);
}

// Switching mode rebuilds the pool at the new size and frees the targets of the old one, and the
// terrain drawables that sampled them: back in the first mode, as many textures are alive as before.
TEST_F(TerrainDrapePool, ModeSwitchRebuildsPoolWithoutLeak) {
    DrapeModeScene scene;
    scene.render(TerrainLoadMode::Quality);
    const int texturesAtFirst = scene.render(TerrainLoadMode::Quality);
    EXPECT_TRUE(allTargetsAre(scene.targetSizes(), 1024));

    scene.render(TerrainLoadMode::Performance);
    EXPECT_TRUE(allTargetsAre(scene.targetSizes(), 512));

    scene.render(TerrainLoadMode::Quality);
    EXPECT_TRUE(allTargetsAre(scene.targetSizes(), 1024));
    EXPECT_EQ(texturesAtFirst, scene.render(TerrainLoadMode::Quality));
}

// A draped drawable outlives the drape targets it drew into: a mode switch drops every target
// (and frees its texture) between one frame and the next, while the hillshade drawables stay and
// draw into the new targets. A drawable that kept a reference to the last target it drew into read
// that freed texture on its next draw (a heap-use-after-free that AddressSanitizer reports here).
TEST_F(TerrainDrapePool, DrawablesOutliveTheTargetsAModeSwitchFrees) {
    DrapeModeScene scene(drapedHillshadeStyle);
    scene.render(TerrainLoadMode::Quality);
    scene.render(TerrainLoadMode::Quality);
    scene.render(TerrainLoadMode::Performance);
    EXPECT_TRUE(allTargetsAre(scene.targetSizes(), 512));
    scene.render(TerrainLoadMode::Quality);
    EXPECT_TRUE(allTargetsAre(scene.targetSizes(), 1024));
}
