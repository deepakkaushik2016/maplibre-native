#include <mln/test/util.hpp>
#include <mln/test/map_adapter.hpp>
#include <mln/test/stub_file_source.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/context.hpp>
#include <mln/gfx/dynamic_texture_atlas.hpp>
#include <mln/gfx/headless_backend.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/image.hpp>
#include <mln/style/image_impl.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/style/style.hpp>
#include <mln/util/run_loop.hpp>

#include <atomic>
#include <map>
#include <set>
#include <tuple>
#include <random>
#include <thread>
#include <vector>

using namespace mln;

namespace {

Immutable<style::Image::Impl> makeImage(const std::string& id, bool sdf, Size size = {16, 16}) {
    return makeMutable<style::Image::Impl>(id, PremultipliedImage(size), 1.0f, sdf);
}

class DynamicTextureAtlasTest : public ::testing::Test {
protected:
    std::unique_ptr<gfx::HeadlessBackend> backend = gfx::HeadlessBackend::Create({64, 64});
    gfx::BackendScope scope{*backend->getRendererBackend()};
    gfx::DynamicTextureAtlas atlas{backend->getRendererBackend()->getContext()};
    std::vector<gfx::ImageAtlas> atlases;

    const gfx::ImageAtlas& uploadPattern(const Immutable<style::Image::Impl>& image) {
        atlases.push_back(atlas.uploadIconsAndPatterns({}, {{image->id, image}}, {}));
        return atlases.back();
    }

    void TearDown() override {
        for (const auto& imageAtlas : atlases) {
            atlas.removeTextures(imageAtlas.textureHandles, imageAtlas.dynamicTexture);
        }
    }
};

} // namespace

TEST_F(DynamicTextureAtlasTest, SharesAllocationWithinImageRevision) {
    const auto image = makeImage("pattern", false);

    const auto first = uploadPattern(image).textureHandles.at(0);
    const auto second = uploadPattern(image).textureHandles.at(0);

    EXPECT_EQ(first.getId(), second.getId());
    EXPECT_EQ(first.getRectangle(), second.getRectangle());
}

TEST_F(DynamicTextureAtlasTest, ReplacedSameSizeImageGetsFreshAllocation) {
    // Each revision keeps its layout alive, as a tile still rendering an older layout would.
    const auto original = uploadPattern(makeImage("pattern", false)).textureHandles.at(0);
    const auto sdf = uploadPattern(makeImage("pattern", true)).textureHandles.at(0);
    const auto toggledBack = uploadPattern(makeImage("pattern", false)).textureHandles.at(0);

    EXPECT_NE(original.getRectangle(), sdf.getRectangle());
    EXPECT_NE(original.getRectangle(), toggledBack.getRectangle());
    EXPECT_NE(sdf.getRectangle(), toggledBack.getRectangle());
}

TEST_F(DynamicTextureAtlasTest, IconAndPatternUsageDoNotShareAllocation) {
    const auto image = makeImage("image", false);

    atlases.push_back(atlas.uploadIconsAndPatterns({{image->id, image}}, {{image->id, image}}, {}));
    const auto& imageAtlas = atlases.back();

    ASSERT_EQ(2u, imageAtlas.textureHandles.size());
    EXPECT_NE(imageAtlas.textureHandles[0].getRectangle(), imageAtlas.textureHandles[1].getRectangle());
}

TEST_F(DynamicTextureAtlasTest, ReleasedRevisionGetsFreshAllocation) {
    const auto image = makeImage("pattern", false);
    const auto released = atlas.uploadIconsAndPatterns({}, {{image->id, image}}, {});
    const auto releasedId = released.textureHandles.at(0).getId();
    atlas.removeTextures(released.textureHandles, released.dynamicTexture);

    // The revision was forgotten on its last release, so it is packed and uploaded again under a new id.
    EXPECT_NE(releasedId, uploadPattern(image).textureHandles.at(0).getId());
}

// Released bins and frames in flight. A layout releases its handles when its tile goes, on a worker
// or the render thread, while frames that drew the tile may still be on the GPU. The bin must not
// be packed again until they have completed: the next image is written into it at once (on Metal,
// by the worker, with no wait), and those frames would sample it. These cases drive the GPU's
// progress by hand.
namespace {

class SerialTexture : public gfx::DynamicTexture {
public:
    SerialTexture(Size size, std::shared_ptr<gfx::FrameSerials> serials)
        : gfx::DynamicTexture(size, gfx::TexturePixelType::Alpha, std::move(serials)) {}

    std::vector<int32_t> released;

protected:
    void onBinReleased(const gfx::TextureHandle& texHandle) override { released.push_back(texHandle.getId()); }
};

struct Serials {
    std::shared_ptr<gfx::FrameSerials> serials = std::make_shared<gfx::FrameSerials>();

    // The backend hands out frame `frame`, and the GPU has completed every frame up to `completed`.
    void at(uint64_t frame, uint64_t completed) const {
        serials->setSubmitted(frame);
        serials->setCompleted(completed);
    }
};

constexpr Size bin{16, 16};

} // namespace

TEST(DynamicTextureFrames, AReleasedBinIsPackedAgainOnlyOnceItsFramesHaveCompleted) {
    Serials gpu;
    SerialTexture texture(bin, gpu.serials);
    gpu.at(4, 2); // frame 4 is being encoded; 3 is on the GPU

    const auto drawn = texture.reserveSize(bin, -1);
    ASSERT_TRUE(drawn);
    EXPECT_TRUE(texture.removeTexture(*drawn)) << "the last handle's release is not reported";
    EXPECT_TRUE(texture.isEmpty());
    EXPECT_EQ(texture.numPendingBins(), 1u);

    EXPECT_FALSE(texture.reserveSize(bin, -1)) << "the bin was packed again while frame 4 may draw it";
    gpu.at(5, 4);
    EXPECT_FALSE(texture.reserveSize(bin, -1)) << "the bin was packed again while frame 5 may draw it";
    EXPECT_TRUE(texture.released.empty());

    gpu.at(6, 5);
    const auto reused = texture.reserveSize(bin, -1);
    ASSERT_TRUE(reused) << "the bin never came back once its frames had completed";
    EXPECT_EQ(reused->getRectangle(), drawn->getRectangle());
    EXPECT_TRUE(reused->isUploadNeeded());
    EXPECT_EQ(texture.released, std::vector<int32_t>{drawn->getId()});
    EXPECT_EQ(texture.numPendingBins(), 0u);
    EXPECT_EQ(gpu.serials->pendingReleases(), 0);
}

TEST(DynamicTextureFrames, TheFrameStartRetiresCompletedBinsWithoutAnAllocation) {
    Serials gpu;
    SerialTexture texture(bin, gpu.serials);
    gpu.at(1, 0);
    const auto drawn = texture.reserveSize(bin, -1);
    ASSERT_TRUE(drawn);
    texture.removeTexture(*drawn);

    texture.retireCompletedBins();
    EXPECT_EQ(texture.numPendingBins(), 1u);
    gpu.at(3, 2);
    texture.retireCompletedBins();
    EXPECT_EQ(texture.numPendingBins(), 0u);
    EXPECT_EQ(texture.released, std::vector<int32_t>{drawn->getId()});
}

// WebGPU (whose queue orders a write after the draws before it) has no frame serials.
TEST(DynamicTextureFrames, WithoutFrameSerialsABinIsFreeAtOnce) {
    SerialTexture texture(bin, nullptr);
    const auto drawn = texture.reserveSize(bin, -1);
    ASSERT_TRUE(drawn);
    EXPECT_TRUE(texture.removeTexture(*drawn));
    EXPECT_EQ(texture.numPendingBins(), 0u);
    EXPECT_TRUE(texture.reserveSize(bin, -1));
}

// A glyph has the same id in every layout. Reserved again while its bin waits, it gets that bin
// back, still holding its pixels: no upload, nothing written under a frame in flight.
TEST(DynamicTextureFrames, AGlyphReservedAgainWhileItsBinWaitsGetsItBackWithoutAnUpload) {
    Serials gpu;
    SerialTexture texture({32, 16}, gpu.serials);
    gpu.at(1, 0);
    const auto first = texture.reserveSize(bin, 7);
    ASSERT_TRUE(first && first->isUploadNeeded());
    texture.removeTexture(*first);

    const auto again = texture.reserveSize(bin, 7);
    ASSERT_TRUE(again);
    EXPECT_FALSE(again->isUploadNeeded()) << "the bin would be written again under a frame in flight";
    EXPECT_EQ(again->getRectangle(), first->getRectangle());
    EXPECT_EQ(texture.numPendingBins(), 0u);
    EXPECT_FALSE(texture.isEmpty());

    // The bin is live again, so its old release must not free it once its frames complete.
    gpu.at(5, 4);
    texture.retireCompletedBins();
    EXPECT_TRUE(texture.released.empty()) << "a bin in use went back to the shelf pack";
    const auto other = texture.reserveSize(bin, -1);
    ASSERT_TRUE(other);
    EXPECT_NE(other->getRectangle(), again->getRectangle());

    // Released again, it waits for the frames of the new release.
    EXPECT_TRUE(texture.removeTexture(*again));
    gpu.at(6, 5);
    texture.retireCompletedBins();
    EXPECT_TRUE(texture.released.empty());
    gpu.at(7, 6);
    texture.retireCompletedBins();
    EXPECT_EQ(texture.released, std::vector<int32_t>{7});
    EXPECT_EQ(gpu.serials->pendingReleases(), 0);
}

// A glyph released, reserved again, and released again later waits for the later release's frames,
// not the first's.
TEST(DynamicTextureFrames, ABinReleasedAgainWaitsForTheLaterRelease) {
    Serials gpu;
    SerialTexture texture(bin, gpu.serials);
    gpu.at(1, 0);
    const auto first = texture.reserveSize(bin, 7);
    ASSERT_TRUE(first);
    texture.removeTexture(*first); // stamped 2
    const auto again = texture.reserveSize(bin, 7);
    ASSERT_TRUE(again);
    gpu.at(3, 1);
    texture.removeTexture(*again); // stamped 4

    gpu.at(3, 2);
    texture.retireCompletedBins();
    EXPECT_TRUE(texture.released.empty()) << "the bin went back at the first release's frames";
    EXPECT_FALSE(texture.reserveSize(bin, -1));
    gpu.at(4, 4);
    texture.retireCompletedBins();
    EXPECT_EQ(texture.released, std::vector<int32_t>{7});
    EXPECT_EQ(texture.numPendingBins(), 0u);
}

// An atlas that can't fit a tile's images in a texture rolls back what it reserved there and moves
// on. Those bins were never uploaded or drawn, so they are free at once: holding them for the
// frames would only crowd the texture.
TEST(DynamicTextureFrames, AReservationRolledBackBeforeUseIsFreeAtOnce) {
    Serials gpu;
    SerialTexture texture(bin, gpu.serials);
    gpu.at(1, 0);
    const auto fresh = texture.reserveSize(bin, 3);
    ASSERT_TRUE(fresh);
    EXPECT_TRUE(texture.releaseUnusedReservation(*fresh));
    EXPECT_EQ(texture.numPendingBins(), 0u);
    EXPECT_TRUE(texture.reserveSize(bin, -1)) << "a rolled-back fresh bin was held for the frames";
}

// A rolled-back handle that shared an existing bin (one a frame may have drawn) still waits.
TEST(DynamicTextureFrames, ARolledBackHandleToABinAFrameMayHaveDrawnStillWaits) {
    Serials gpu;
    SerialTexture texture(bin, gpu.serials);
    gpu.at(1, 0);
    const auto drawn = texture.reserveSize(bin, 3);
    ASSERT_TRUE(drawn);
    texture.removeTexture(*drawn);
    const auto shared = texture.reserveSize(bin, 3); // the glyph again, from a pending bin
    ASSERT_TRUE(shared && !shared->isUploadNeeded());

    EXPECT_TRUE(texture.releaseUnusedReservation(*shared));
    EXPECT_EQ(texture.numPendingBins(), 1u);
    EXPECT_FALSE(texture.reserveSize(bin, -1));
}

// Tiles come and go every frame while two frames are in flight; the texture holds exactly as many
// images as fit. No allocation may fail beyond what the frames hold back, and once the GPU is idle
// every bin is free again.
TEST(DynamicTextureFrames, FreeingAndPackingUnderPressureLeaksNoBin) {
    constexpr Size size{128, 128};
    constexpr Size image{8, 8};
    constexpr size_t capacity = (128 / 8) * (128 / 8);
    Serials gpu;
    SerialTexture texture(size, gpu.serials);
    std::mt19937 random(29);

    std::vector<gfx::TextureHandle> live;
    std::vector<size_t> releasedPerFrame(1, 0);
    for (uint64_t frame = 1; frame <= 400; ++frame) {
        gpu.at(frame, frame > 3 ? frame - 3 : 0);
        texture.retireCompletedBins();
        releasedPerFrame.push_back(0);

        // Bins released in frames whose stamps have not completed yet: frames frame-2..frame, the
        // releases stamped frame-1..frame+1.
        size_t held = 0;
        for (uint64_t f = frame > 2 ? frame - 2 : 1; f <= frame; ++f) {
            held += releasedPerFrame[f - 1];
        }
        EXPECT_EQ(texture.numPendingBins(), held) << "frame " << frame;

        // Fill what is free, then release a random part of what is live.
        const size_t free = capacity - live.size() - texture.numPendingBins();
        for (size_t i = 0; i < free; ++i) {
            const auto handle = texture.reserveSize(image, -1);
            ASSERT_TRUE(handle) << "frame " << frame << ": a free bin could not be packed";
            live.push_back(*handle);
        }
        EXPECT_FALSE(texture.reserveSize(image, -1)) << "frame " << frame << ": more bins than fit";
        std::shuffle(live.begin(), live.end(), random);
        const size_t release = std::uniform_int_distribution<size_t>(0, live.size() / 2)(random);
        for (size_t i = 0; i < release; ++i) {
            EXPECT_TRUE(texture.removeTexture(live.back()));
            live.pop_back();
        }
        releasedPerFrame[frame] = release;
    }

    for (const auto& handle : live) {
        texture.removeTexture(handle);
    }
    live.clear();
    gpu.at(402, 402); // idle
    texture.retireCompletedBins();
    EXPECT_EQ(texture.numPendingBins(), 0u);
    EXPECT_EQ(gpu.serials->pendingReleases(), 0);
    EXPECT_TRUE(texture.isEmpty());
    for (size_t i = 0; i < capacity; ++i) {
        ASSERT_TRUE(texture.reserveSize(image, -1)) << "bin " << i << " leaked";
    }
}

// The same from worker threads, while the render thread moves the frames on and retires bins. The
// workers also reserve a few glyph ids that every layout shares, so a glyph is released and
// reserved again, by different threads, while its bin waits. No bin may be lost or handed out
// twice, and once idle, every bin is free again.
TEST(DynamicTextureFrames, WorkersFreeAndPackWhileFramesAdvance) {
    constexpr Size size{128, 128};
    constexpr Size image{8, 8};
    constexpr size_t capacity = (128 / 8) * (128 / 8);
    constexpr int32_t glyphIds = 16; // ids 1..16; shelf-pack ids for fresh bins start above them
    Serials gpu;
    SerialTexture texture(size, gpu.serials);
    for (int32_t id = 1; id <= glyphIds; ++id) { // reserve the glyph ids once, so fresh ids pass them
        texture.removeTexture(*texture.reserveSize(image, id));
    }
    std::atomic<bool> stop{false};
    std::atomic<size_t> operations{0};
    std::atomic<size_t> resurrected{0};

    std::vector<std::vector<gfx::TextureHandle>> held(4);
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            std::mt19937 random(worker);
            auto& live = held[worker];
            while (!stop) {
                const bool glyph = random() % 2;
                const int32_t id = glyph ? static_cast<int32_t>(1 + random() % glyphIds) : -1;
                if (auto handle = texture.reserveSize(image, id)) {
                    if (glyph && !handle->isUploadNeeded()) {
                        ++resurrected;
                    }
                    live.push_back(*handle);
                }
                if (!live.empty() && random() % 2) {
                    const auto index = random() % live.size();
                    texture.removeTexture(live[index]);
                    live.erase(live.begin() + index);
                }
                ++operations;
            }
        });
    }
    uint64_t frame = 0;
    while (++frame < 2000 || operations < 200000) {
        gpu.at(frame, frame > 2 ? frame - 2 : 0);
        texture.retireCompletedBins();
    }
    stop = true;
    for (auto& worker : workers) {
        worker.join();
    }
    EXPECT_GT(resurrected, 0u) << "no glyph was reserved again while shared or waiting (the case tests nothing)";

    // Every live handle has a bin of its own, or shares one only with handles of the same id.
    std::map<int32_t, Rect<uint16_t>> rectOf;
    std::set<std::tuple<uint16_t, uint16_t>> origins;
    for (const auto& live : held) {
        for (const auto& handle : live) {
            const auto [it, added] = rectOf.emplace(handle.getId(), handle.getRectangle());
            EXPECT_EQ(it->second, handle.getRectangle()) << "one id, two bins";
            if (added) {
                EXPECT_TRUE(origins.emplace(handle.getRectangle().x, handle.getRectangle().y).second)
                    << "a bin was handed out twice";
            }
        }
    }

    for (const auto& live : held) {
        for (const auto& handle : live) {
            texture.removeTexture(handle);
        }
    }
    gpu.at(frame + 2, frame + 2);
    texture.retireCompletedBins();
    EXPECT_EQ(texture.numPendingBins(), 0u);
    EXPECT_EQ(gpu.serials->pendingReleases(), 0);
    EXPECT_TRUE(texture.isEmpty());
    for (size_t i = 0; i < capacity; ++i) {
        ASSERT_TRUE(texture.reserveSize(image, -1)) << "bin " << i << " leaked";
    }
}

// The atlas rolls back an icon set that doesn't fit its first texture: the small icon reserved
// there must not stay held for frames that never drew it.
TEST_F(DynamicTextureAtlasTest, AnIconSetRolledBackFromAFullTextureHoldsNoBin) {
    ASSERT_TRUE(backend->getRendererBackend()->getContext().getFrameSerials());
    uploadPattern(makeImage("pattern", false)); // the first texture exists, and stays
    // Reserved in id order: the small icon first, then the large one, which fits only the second,
    // 1024² texture.
    const auto small = makeImage("a-small", false, {8, 8});
    const auto large = makeImage("b-large", false, {600, 600});
    atlases.push_back(atlas.uploadIconsAndPatterns({{small->id, small}, {large->id, large}}, {}, {}));

    ASSERT_EQ(atlases.back().textureHandles.size(), 2u);
    ASSERT_NE(atlases.back().dynamicTexture, atlases.front().dynamicTexture);
    EXPECT_EQ(atlas.numPendingBins(), 0u) << "a bin reserved and rolled back waits for the frames";
}

// The same on the glyph path: a glyph set that doesn't fit the first glyph texture rolls back the
// glyph it reserved there, which must not stay held for frames that never drew it.
TEST_F(DynamicTextureAtlasTest, AGlyphSetRolledBackFromAFullTextureHoldsNoBin) {
    ASSERT_TRUE(backend->getRendererBackend()->getContext().getFrameSerials());
    const auto glyph = [](uint32_t id, Size size) {
        auto made = makeMutable<Glyph>();
        made->id = GlyphID(id);
        made->bitmap = AlphaImage(size);
        return Immutable<Glyph>(std::move(made));
    };
    const FontStackHash fontStack = 4;
    std::vector<gfx::GlyphAtlas> glyphAtlases;
    // The first glyph texture exists, and stays.
    glyphAtlases.push_back(atlas.uploadGlyphs({{fontStack, {{GlyphID(65u), glyph(65, {8, 8})}}}}));
    // A small glyph is reserved there first, then a large one that fits only the second texture.
    glyphAtlases.push_back(
        atlas.uploadGlyphs({{fontStack, {{GlyphID(66u), glyph(66, {8, 8})}, {GlyphID(67u), glyph(67, {600, 600})}}}}));

    ASSERT_EQ(glyphAtlases.back().textureHandles.size(), 2u);
    ASSERT_NE(glyphAtlases.back().dynamicTexture, glyphAtlases.front().dynamicTexture);
    EXPECT_EQ(atlas.numPendingBins(), 0u) << "a glyph bin reserved and rolled back waits for the frames";
    for (const auto& glyphAtlas : glyphAtlases) {
        atlas.removeTextures(glyphAtlas.textureHandles, glyphAtlas.dynamicTexture);
    }
}

// A map whose icons go away releases their bins; as frames go on they come back, with nothing
// left waiting once the map is idle.
TEST(DynamicTextureFrames, AMapReleasesItsAtlasBinsOnceItsFramesHaveCompleted) {
    util::RunLoop loop;
    HeadlessFrontend frontend{{256, 256}, 1};
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions()),
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()));
    map.getStyle().loadJSON(R"STYLE({
      "version": 8,
      "sources": {"points": {"type": "geojson", "data": {"type": "Feature", "properties": {},
                  "geometry": {"type": "Point", "coordinates": [0, 0]}}}},
      "layers": [{"id": "icons", "type": "symbol", "source": "points",
                  "layout": {"icon-image": "dot", "icon-allow-overlap": true}}]
    })STYLE");
    PremultipliedImage dot({8, 8});
    dot.fill(255);
    map.getStyle().addImage(std::make_unique<style::Image>("dot", std::move(dot), 1.0f));
    map.jumpTo(CameraOptions().withCenter(LatLng{0.0, 0.0}).withZoom(2.0));
    const auto serials = [&] {
        gfx::BackendScope guard{*frontend.getBackend()};
        return frontend.getBackend()->getContext().getFrameSerials();
    }();
    ASSERT_TRUE(serials);

    frontend.render(map);
    const auto pendingBefore = serials->pendingReleases();

    // The tile is laid out again with no icon; its earlier layout goes, with its handles.
    map.getStyle().getSource("points")->as<style::GeoJSONSource>()->setGeoJSON(
        mapbox::feature::feature_collection<double>{});
    int64_t pendingSeen = 0;
    for (int frame = 0; frame < 6; ++frame) {
        map.triggerRepaint();
        frontend.render(map);
        pendingSeen = std::max(pendingSeen, serials->pendingReleases());
    }
    EXPECT_GT(pendingSeen, pendingBefore) << "the layer's bins were not released (the case tests nothing)";
    EXPECT_EQ(serials->pendingReleases(), 0) << "released bins still wait after the frames completed";
}

#if MLN_RENDER_BACKEND_OPENGL
// OpenGL writes a bin again only on the render thread, in a frame's upload pass, after the draws
// issued before it; so a bin released in frame 2 (stamped 3) is free from frame 4, once frame 3 has
// been issued, with no wait for the GPU.
TEST(DynamicTextureFrames, OnOpenGLABinComesBackOnceItsFramesHaveBeenIssued) {
    auto backend = gfx::HeadlessBackend::Create({64, 64});
    gfx::BackendScope scope{*backend->getRendererBackend()};
    auto& context = backend->getRendererBackend()->getContext();
    auto atlas = context.createDynamicTexture({16, 16}, gfx::TexturePixelType::Alpha);
    std::vector<uint8_t> pixels(16 * 16, 0xff);

    context.beginFrame(); // frame 1
    const auto drawn = atlas->addImage(pixels.data(), {16, 16});
    ASSERT_TRUE(drawn);
    context.endFrame();

    context.beginFrame(); // frame 2
    atlas->removeTexture(*drawn);
    int frame = 2;
    std::optional<gfx::TextureHandle> reused;
    while (!(reused = atlas->reserveSize({16, 16}, -1)) && frame < 8) {
        context.endFrame();
        context.beginFrame();
        ++frame;
    }
    context.endFrame();
    ASSERT_TRUE(reused) << "the bin never came back";
    EXPECT_EQ(frame, 4) << "the bin came back in the wrong frame";
}
#endif
