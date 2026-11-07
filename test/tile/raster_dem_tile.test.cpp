#include <mln/test/util.hpp>
#include <mln/test/fake_file_source.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/tile/raster_dem_tile.hpp>
#include <mln/tile/tile_loader_impl.hpp>

#include <mln/style/style.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/map/transform.hpp>
#include <mln/annotation/annotation_manager.hpp>
#include <mln/renderer/tile_parameters.hpp>
#include <mln/renderer/buckets/hillshade_bucket.hpp>
#include <mln/renderer/image_manager.hpp>
#include <mln/text/glyph_manager.hpp>
#include <mln/gfx/dynamic_texture_atlas.hpp>

using namespace mln;

class RasterDEMTileTest {
public:
    util::SimpleIdentity uniqueID;
    std::shared_ptr<FileSource> fileSource;
    TransformState transformState;
    util::RunLoop loop;
    AnnotationManager annotationManager{style};
    std::shared_ptr<ImageManager> imageManager = ImageManager::create();
    std::shared_ptr<GlyphManager> glyphManager = std::make_shared<GlyphManager>();
    gfx::DynamicTextureAtlasPtr dynamicTextureAtlas;

    Tileset tileset{{"https://example.com"}, {0, 22}, "none"};
    TileParameters tileParameters;
    style::Style style;

    explicit RasterDEMTileTest(std::shared_ptr<FileSource> fileSource_ = std::make_shared<FakeFileSource>())
        : fileSource(std::move(fileSource_)),
          tileParameters{.pixelRatio = 1.0,
                         .debugOptions = MapDebugOptions(),
                         .transformState = transformState,
                         .fileSource = fileSource,
                         .mode = MapMode::Continuous,
                         .annotationManager = annotationManager.makeWeakPtr(),
                         .imageManager = imageManager,
                         .glyphManager = glyphManager,
                         .prefetchZoomDelta = 0,
                         .threadPool = {Scheduler::GetBackground(), uniqueID},
                         .dynamicTextureAtlas = dynamicTextureAtlas},
          style{fileSource, 1, tileParameters.threadPool} {}
};

TEST(RasterDEMTile, setError) {
    RasterDEMTileTest test;
    RasterDEMTile tile(OverscaledTileID(0, 0, 0), "testSource", test.tileParameters, test.tileset);
    tile.setError(std::make_exception_ptr(std::runtime_error("test")));
    EXPECT_FALSE(tile.isRenderable());
    EXPECT_TRUE(tile.isLoaded());
    EXPECT_TRUE(tile.isComplete());
}

TEST(RasterDEMTile, onError) {
    RasterDEMTileTest test;
    RasterDEMTile tile(OverscaledTileID(0, 0, 0), "testSource", test.tileParameters, test.tileset);
    tile.onError(std::make_exception_ptr(std::runtime_error("test")), 0);
    EXPECT_FALSE(tile.isRenderable());
    EXPECT_TRUE(tile.isLoaded());
    EXPECT_TRUE(tile.isComplete());
}

TEST(RasterDEMTile, onParsed) {
    RasterDEMTileTest test;
    RasterDEMTile tile(OverscaledTileID(0, 0, 0), "testSource", test.tileParameters, test.tileset);
    tile.onParsed(std::make_unique<HillshadeBucket>(PremultipliedImage({16, 16}), Tileset::RasterEncoding::Mapbox), 0);
    EXPECT_TRUE(tile.isRenderable());
    EXPECT_TRUE(tile.isLoaded());
    EXPECT_TRUE(tile.isComplete());

    // Make sure that once we've had a renderable tile and then receive
    // erroneous data, we retain the previously rendered data and keep the tile
    // renderable.
    tile.setError(std::make_exception_ptr(std::runtime_error("Connection offline")));
    EXPECT_TRUE(tile.isRenderable());
    EXPECT_TRUE(tile.isLoaded());
    EXPECT_TRUE(tile.isComplete());

    // Then simulate a parsing failure and make sure that we keep it renderable
    // in this situation as well.
    tile.onError(std::make_exception_ptr(std::runtime_error("Parse error")), 0);
    ASSERT_TRUE(tile.isRenderable());
    EXPECT_TRUE(tile.isLoaded());
    EXPECT_TRUE(tile.isComplete());
}

TEST(RasterDEMTile, onParsedEmpty) {
    RasterDEMTileTest test;
    RasterDEMTile tile(OverscaledTileID(0, 0, 0), "testSource", test.tileParameters, test.tileset);
    tile.onParsed(nullptr, 0);
    EXPECT_FALSE(tile.isRenderable());
    EXPECT_TRUE(tile.isLoaded());
    EXPECT_TRUE(tile.isComplete());
}

namespace {

// Answers from a cache (empty: every tile is "not cached") and a network (204 for every tile), as
// DefaultFileSource does, counting the requests of each.
class CountingFileSource : public StubFileSource {
public:
    CountingFileSource()
        : StubFileSource(ResourceOptions::Default(), ClientOptions()) {
        tileResponse = [this](const Resource& resource) -> std::optional<Response> {
            Response res;
            if (resource.loadingMethod == Resource::LoadingMethod::CacheOnly) {
                ++cacheRequests;
                res.error = std::make_unique<Response::Error>(Response::Error::Reason::NotFound, "not cached");
            } else {
                ++networkRequests;
                res.noContent = true;
            }
            return res;
        };
    }
    bool supportsCacheOnlyRequests() const override { return true; }
    int cacheRequests = 0;
    int networkRequests = 0;
};

} // namespace

// A tile known to be empty that is required again as the same object (held meanwhile as an optional
// parent or child, as the tile cache keeps no tile without data) asks the cache again, once, and
// not the network, while the record holds; after it lapses, the network.
TEST(RasterDEMTile, AKnownEmptyTileRequiredAgainAsksOnlyTheCache) {
    auto fileSource = std::make_shared<CountingFileSource>();
    RasterDEMTileTest test(fileSource);
    const OverscaledTileID id(13, 4328, 2896);
    auto record = std::make_shared<KnownEmptyDEMTiles>();
    record->add(id.canonical, std::nullopt);
    RasterDEMTile tile(id, "testSource", test.tileParameters, test.tileset, nullptr, record);
    EXPECT_TRUE(tile.isLoaded()) << "a known-empty tile starts out loaded";

    const auto settle = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (std::chrono::steady_clock::now() < deadline) {
            test.loop.runOnce();
        }
    };
    settle();
    ASSERT_EQ(fileSource->cacheRequests, 1) << "a new tile asks the cache, as any tile does";

    tile.setNecessity(TileNecessity::Required);
    settle();
    EXPECT_EQ(fileSource->cacheRequests, 2) << "required, it does not ask the cache";
    tile.setNecessity(TileNecessity::Required);
    settle();
    EXPECT_EQ(fileSource->cacheRequests, 2) << "it asks the cache on every call, not once per revisit";

    tile.setNecessity(TileNecessity::Optional);
    tile.setNecessity(TileNecessity::Required);
    settle();
    EXPECT_EQ(fileSource->cacheRequests, 3);
    EXPECT_EQ(fileSource->networkRequests, 0) << "it asks the network while the record holds";

    record->remove(id.canonical);
    tile.setNecessity(TileNecessity::Optional);
    tile.setNecessity(TileNecessity::Required);
    settle();
    EXPECT_GE(fileSource->networkRequests, 1) << "the record lapsed, and the network is not asked";
}
