#pragma once

#include <mln/annotation/annotation.hpp>
#include <mln/renderer/query.hpp>
#include <mln/util/geo.hpp>
#include <mln/util/geojson.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mln {

class RendererObserver;
class RenderedQueryOptions;
class SourceQueryOptions;
class UpdateParameters;

namespace gfx {
class RendererBackend;
} // namespace gfx

struct PlacedSymbolData {
    /// Contents of the label
    std::u16string key;
    /// If symbol contains text, text collision box in viewport coordinates
    std::optional<mapbox::geometry::box<float>> textCollisionBox;
    /// If symbol contains icon, icon collision box in viewport coordinates
    std::optional<mapbox::geometry::box<float>> iconCollisionBox;
    /// Symbol text was placed
    bool textPlaced;
    /// Symbol icon was placed
    bool iconPlaced;
    /// Symbol text or icon collision box intersects tile borders
    bool intersectsTileBorder;
    /// Viewport padding ({viewportPadding, viewportPadding} is a coordinate of the tile's top-left corner)
    float viewportPadding;
    /// Layer id (leader of the symbol layout group)
    std::string layer;
};

class Renderer {
public:
    Renderer(gfx::RendererBackend&,
             float pixelRatio_,
             const std::optional<std::string>& localFontFamily = std::nullopt);
    ~Renderer();

    void markContextLost();

    void setObserver(RendererObserver*);

    void render(const std::shared_ptr<UpdateParameters>&);

    /// Feature queries
    std::vector<Feature> queryRenderedFeatures(const ScreenLineString&, const RenderedQueryOptions& options = {}) const;
    std::vector<Feature> queryRenderedFeatures(const ScreenCoordinate& point,
                                               const RenderedQueryOptions& options = {}) const;
    std::vector<Feature> queryRenderedFeatures(const ScreenBox& box, const RenderedQueryOptions& options = {}) const;
    std::vector<Feature> querySourceFeatures(const std::string& sourceID, const SourceQueryOptions& options = {}) const;
    AnnotationIDs queryPointAnnotations(const ScreenBox& box) const;
    AnnotationIDs queryShapeAnnotations(const ScreenBox& box) const;
    AnnotationIDs getAnnotationIDs(const std::vector<Feature>&) const;

    /**
     * @brief The terrain elevation at `point`, as drawn.
     *
     * Metres above sea level with the style's terrain exaggeration applied, i.e. the height the
     * terrain surface is rendered at (a 1000 m peak under exaggeration 2 reads 2000). Sampled
     * bilinearly from the finest DEM tile currently loaded, or its closest loaded ancestor.
     * Longitudes are taken in the world copy nearest the map's centre, where the last rendered
     * frame draws them.
     *
     * @return std::nullopt when the style has no terrain, or no loaded DEM tile covers the point
     * (for instance before the first DEM tile arrives).
     */
    std::optional<double> queryTerrainElevation(const LatLng& point) const;

    /**
     * @brief The ground drawn under a screen point.
     *
     * What `Map::latLngForPixel` answers at sea level, for the terrain surface as drawn: the
     * line of sight through `point` is raycast against the loaded DEM (exaggeration applied),
     * in the transform of the last rendered frame. Read the height there with
     * queryTerrainElevation.
     *
     * Without terrain in the style, it is `Map::latLngForPixel` for the last rendered frame's
     * transform, bit for bit.
     *
     * @param point screen pixels, top-left origin (as `Map::latLngForPixel` takes it)
     * @return the point on the ground, wrapped to [-180, 180]; std::nullopt before the first
     * frame (no transform to read), for a non-finite point, or, with terrain, when no DEM tile
     * is loaded yet or the line of sight meets no ground (sky above the horizon, or ground no
     * loaded DEM tile covers).
     */
    std::optional<LatLng> queryTerrainLatLng(const ScreenCoordinate& point) const;

    /// Where a point on the ground is drawn (queryTerrainPixel).
    struct TerrainPixel {
        /// Screen pixels, top-left origin (as `Map::pixelForLatLng` returns it). It may lie
        /// outside the viewport.
        ScreenCoordinate point;
        /// Ground from the loaded DEM is drawn in front of the point and hides it, as a ridge
        /// hides the valley behind it. See queryTerrainPixel for what it does not see.
        bool occluded = false;
    };

    /**
     * @brief Where a point on the ground is drawn on the screen.
     *
     * What `Map::pixelForLatLng` answers at sea level, for the point lifted to the height the
     * terrain is drawn at there (queryTerrainElevation), projected with the transform of the
     * last rendered frame. Longitudes are taken in the world copy nearest the map's centre,
     * where the frame draws them.
     *
     * `occluded` is true only when ground from the loaded DEM is drawn in front of the point:
     * the line of sight through its pixel, raycast as queryTerrainLatLng does, meets loaded-DEM
     * ground more than one world pixel (a pixel of the map at its zoom) nearer the camera than
     * the point. Otherwise it is false, which is not a proof that nothing hides the point:
     * - ground no loaded DEM tile covers never occludes (the frame draws a stand-in height
     *   there until its DEM arrives);
     * - a line of sight the raycast does not see meet the ground reports the point unoccluded.
     *   The raycast samples it every 4 world pixels across the ground, or every 1/512 of its
     *   stretch within the loaded DEM's height range if that is longer, so it can step over a
     *   crest thinner than that on a long, grazing line of sight.
     *
     * Without terrain in the style, `point` is `Map::pixelForLatLng` for the last rendered
     * frame's transform, bit for bit, and `occluded` is false. (Unless LatLngBounds hold the
     * centre outside [-180, 180]: `Map::pixelForLatLng` can then project a world copy that is
     * not drawn, while this projects the one that is.)
     *
     * @return std::nullopt before the first frame (no transform to read), when the point is
     * behind the camera (no pixel draws it; `Map::pixelForLatLng` answers a mirrored point
     * there), or, with terrain, when no loaded DEM tile covers the point.
     */
    std::optional<TerrainPixel> queryTerrainPixel(const LatLng& point) const;

    /// Feature extension query
    FeatureExtensionValue queryFeatureExtensions(
        const std::string& sourceID,
        const Feature& feature,
        const std::string& extension,
        const std::string& extensionField,
        const std::optional<std::map<std::string, Value>>& args = std::nullopt) const;

    void setFeatureState(const std::string& sourceID,
                         const std::optional<std::string>& sourceLayerID,
                         const std::string& featureID,
                         const FeatureState& state);

    void getFeatureState(FeatureState& state,
                         const std::string& sourceID,
                         const std::optional<std::string>& sourceLayerID,
                         const std::string& featureID) const;

    FeatureState getFeatureState(const std::string& sourceID,
                                 const std::optional<std::string>& sourceLayerID,
                                 const std::string& featureID) const;

    void removeFeatureState(const std::string& sourceID,
                            const std::optional<std::string>& sourceLayerID,
                            const std::optional<std::string>& featureID,
                            const std::optional<std::string>& stateKey);

    // Debug
    void dumpDebugLogs();

    /**
     * @brief In Tile map mode, enables or disables collecting of the placed
     * symbols data, which can be obtained with `getPlacedSymbolsData()`.
     *
     * The placed symbols data collecting is disabled by default.
     */
    void collectPlacedSymbolData(bool enable);

    /**
     * @brief If collecting of the placed symbols data is enabled, returns the
     * reference to the `PlacedSymbolData` vector holding the collected data.
     *
     * Note: the returned vector gets re-populated at every `render()` call.
     *
     * @return collected placed symbols data
     */
    const std::vector<PlacedSymbolData>& getPlacedSymbolsData() const;

    // Memory
    void setTileCacheEnabled(bool);
    bool getTileCacheEnabled() const;
    void reduceMemoryUse();
    void clearData();

#if MLN_RENDER_BACKEND_OPENGL
    void enableAndroidEmulatorGoldfishMitigation(bool enable);
#endif

private:
    class Impl;
    std::unique_ptr<Impl> impl;

    // Lets tests inspect the internals (such as the layer groups drawn) without adding API.
    friend class RendererTestAccess;
};

} // namespace mln
