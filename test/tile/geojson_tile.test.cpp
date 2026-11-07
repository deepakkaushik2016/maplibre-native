#include <mln/test/util.hpp>
#include <mln/test/fake_file_source.hpp>
#include <mln/test/stub_tile_observer.hpp>
#include <mln/tile/geojson_tile.hpp>
#include <mln/tile/tile_loader_impl.hpp>

#include <mln/annotation/annotation_manager.hpp>
#include <mln/map/transform.hpp>
#include <mln/renderer/bucket.hpp>
#include <mln/renderer/image_manager.hpp>
#include <mln/renderer/layers/render_circle_layer.hpp>
#include <mln/renderer/property_evaluation_parameters.hpp>
#include <mln/renderer/tile_render_data.hpp>
#include <mln/renderer/tile_parameters.hpp>
#include <mln/style/layers/circle_layer.hpp>
#include <mln/style/layers/circle_layer_impl.hpp>
#include <mln/style/conversion/json.hpp>
#include <mln/style/conversion/property_value.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/style/style.hpp>
#include <mln/text/glyph_manager.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/gfx/dynamic_texture_atlas.hpp>

#include <memory>

using namespace mln;
using namespace mln::style;

class GeoJSONTileTest {
public:
    util::SimpleIdentity uniqueID;
    std::shared_ptr<FileSource> fileSource = std::make_shared<FakeFileSource>();
    TransformState transformState;
    util::RunLoop loop;
    AnnotationManager annotationManager{style};
    std::shared_ptr<ImageManager> imageManager = ImageManager::create();
    std::shared_ptr<GlyphManager> glyphManager = std::make_shared<GlyphManager>();
    gfx::DynamicTextureAtlasPtr dynamicTextureAtlas;

    Tileset tileset{{"https://example.com"}, {0, 22}, "none"};
    TileParameters tileParameters;
    style::Style style;

    GeoJSONTileTest()
        : tileParameters{.pixelRatio = 1.0,
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

namespace {

class FakeGeoJSONData : public GeoJSONData {
public:
    FakeGeoJSONData(TileFeatures features_)
        : features(std::move(features_)) {}

    void getTile(const CanonicalTileID&, const std::function<void(TileFeatures)>& fn, bool) final {
        assert(fn);
        fn(features);
    }

    Features getChildren(const std::uint32_t) final { return {}; }

    Features getLeaves(const std::uint32_t, const std::uint32_t, const std::uint32_t) final { return {}; }

    std::uint8_t getClusterExpansionZoom(std::uint32_t) final { return 0; }

private:
    TileFeatures features;
};

} // namespace

TEST(GeoJSONTile, SynchronousUpdate) {
    GeoJSONTileTest test;

    CircleLayer layer("circle", "source");

    mapbox::feature::feature_collection<int16_t> features;
    features.push_back(mapbox::feature::feature<int16_t>{mapbox::geometry::point<int16_t>(0, 0)});
    auto data = std::make_shared<FakeGeoJSONData>(std::move(features));
    TileParameters tileParameters = test.tileParameters;
    tileParameters.isUpdateSynchronous = true;
    GeoJSONTile tile(OverscaledTileID(0, 0, 0), "source", tileParameters, data);
    Immutable<LayerProperties> layerProperties = makeMutable<CircleLayerProperties>(
        staticImmutableCast<CircleLayer::Impl>(layer.baseImpl));
    std::vector<Immutable<LayerProperties>> layers{layerProperties};
    tile.setLayers(layers, tileParameters.globalState);
    ASSERT_TRUE(tile.isComplete());
    ASSERT_TRUE(tile.isRenderable());
}

TEST(GeoJSONTile, Issue7648) {
    GeoJSONTileTest test;

    CircleLayer layer("circle", "source");

    mapbox::feature::feature_collection<int16_t> features;
    features.push_back(mapbox::feature::feature<int16_t>{mapbox::geometry::point<int16_t>(0, 0)});
    auto data = std::make_shared<FakeGeoJSONData>(std::move(features));
    GeoJSONTile tile(OverscaledTileID(0, 0, 0), "source", test.tileParameters, data);
    Immutable<LayerProperties> layerProperties = makeMutable<CircleLayerProperties>(
        staticImmutableCast<CircleLayer::Impl>(layer.baseImpl));
    StubTileObserver observer;
    observer.tileChanged = [&](const Tile&) {
        // Once present, the bucket should never "disappear", which would cause
        // flickering.
        ASSERT_TRUE(tile.layerPropertiesUpdated(layerProperties));
    };

    std::vector<Immutable<LayerProperties>> layers{layerProperties};
    tile.setLayers(layers, test.tileParameters.globalState);
    tile.setObserver(&observer);

    while (!tile.isComplete()) {
        test.loop.runOnce();
    }

    tile.updateData(data, false, test.tileParameters.isUpdateSynchronous);
    while (!tile.isComplete()) {
        test.loop.runOnce();
    }
}

// Tests that tiles remain renderable if they have been renderable and then had
// an error sent to them, e.g. when revalidating/refreshing the request.
TEST(GeoJSONTile, Issue9927) {
    GeoJSONTileTest test;

    CircleLayer layer("circle", "source");

    mapbox::feature::feature_collection<int16_t> features;
    features.push_back(mapbox::feature::feature<int16_t>{mapbox::geometry::point<int16_t>(0, 0)});
    auto data = std::make_shared<FakeGeoJSONData>(std::move(features));
    GeoJSONTile tile(OverscaledTileID(0, 0, 0), "source", test.tileParameters, data);

    Immutable<LayerProperties> layerProperties = makeMutable<CircleLayerProperties>(
        staticImmutableCast<CircleLayer::Impl>(layer.baseImpl));
    std::vector<Immutable<LayerProperties>> layers{layerProperties};
    tile.setLayers(layers, test.tileParameters.globalState);

    while (!tile.isComplete()) {
        test.loop.runOnce();
    }

    ASSERT_TRUE(tile.isRenderable());
    ASSERT_TRUE(tile.layerPropertiesUpdated(layerProperties));

    // Make sure that once we've had a renderable tile and then receive
    // erroneous data, we retain the previously rendered data and keep the tile
    // renderable.
    tile.setError(std::make_exception_ptr(std::runtime_error("Connection offline")));
    ASSERT_TRUE(tile.isRenderable());
    ASSERT_TRUE(tile.layerPropertiesUpdated(layerProperties));

    // Then simulate a parsing failure and make sure that we keep it renderable
    // in this situation as well. We're using 3 as a correlationID since we've
    // done two three calls that increment this counter (as part of the
    // GeoJSONTile constructor, setLayers, and setPlacementConfig).
    tile.onError(std::make_exception_ptr(std::runtime_error("Parse error")), 3);
    ASSERT_TRUE(tile.isRenderable());
    ASSERT_TRUE(tile.layerPropertiesUpdated(layerProperties));
}

// A tile applies the source's feature states once per revision (GeometryTile::setFeatureState):
// they are sent on every frame once any is set, and re-applying them at rest re-evaluated and
// re-uploaded every bucket. A bucket's feature-state revision, which the terrain drape keys on,
// moves only when a state changed one of its vertices.
TEST(GeoJSONTile, AFeatureStateRevisionIsAppliedOnce) {
    GeoJSONTileTest test;

    CircleLayer layer("circle", "source");
    conversion::Error error;
    const auto color = conversion::convertJSON<PropertyValue<Color>>(
        R"(["case", ["boolean", ["feature-state", "selected"], false], "#0000ff", "#ff0000"])", error, true, false);
    ASSERT_TRUE(color) << error.message;
    layer.setCircleColor(*color);
    RenderCircleLayer circleLayer(staticImmutableCast<CircleLayer::Impl>(layer.baseImpl));
    RenderLayer& renderLayer = circleLayer;
    renderLayer.transition({TimePoint{}, TransitionOptions{}});
    renderLayer.evaluate(PropertyEvaluationParameters(0));

    mapbox::feature::feature_collection<int16_t> features;
    features.push_back(mapbox::feature::feature<int16_t>{
        mapbox::geometry::point<int16_t>(0, 0), {}, mapbox::feature::identifier(uint64_t{7})});
    auto data = std::make_shared<FakeGeoJSONData>(std::move(features));
    TileParameters tileParameters = test.tileParameters;
    tileParameters.isUpdateSynchronous = true;
    GeoJSONTile tile(OverscaledTileID(0, 0, 0), "source", tileParameters, data);
    std::vector<Immutable<LayerProperties>> layers{renderLayer.evaluatedProperties};
    tile.setLayers(layers, tileParameters.globalState);
    ASSERT_TRUE(tile.isRenderable());
    const auto renderData = tile.createRenderData();
    const Bucket* bucket = renderData->getBucket(*layer.baseImpl);
    ASSERT_NE(bucket, nullptr);

    const auto states = [](bool selected) {
        FeatureState state;
        state["selected"] = selected;
        return LayerFeatureStates{{"", FeatureStates{{"7", state}}}};
    };
    EXPECT_EQ(bucket->getFeatureStateRevision(), 0u);
    tile.setFeatureState(states(true), 1);
    EXPECT_EQ(bucket->getFeatureStateRevision(), 1u) << "the state was not applied";
    // The same revision again (a frame at rest): nothing is re-applied, even if the states differ.
    tile.setFeatureState(states(false), 1);
    EXPECT_EQ(bucket->getFeatureStateRevision(), 1u) << "an applied revision was applied again";
    tile.setFeatureState(states(false), 2);
    EXPECT_EQ(bucket->getFeatureStateRevision(), 2u) << "a new revision was not applied";
    // A new revision that leaves this feature's value as it was (a change elsewhere).
    tile.setFeatureState(states(false), 3);
    EXPECT_EQ(bucket->getFeatureStateRevision(), 2u) << "an unchanged value moved the revision";
}
