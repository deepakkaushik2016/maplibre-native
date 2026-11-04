#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/bound_options.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/map/transform_state.hpp>
#include <mln/math/angles.hpp>
#include <mln/renderer/renderer.hpp>
#include <mln/style/layers/circle_layer.hpp>
#include <mln/style/layers/fill_extrusion_layer.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/style/style.hpp>
#include <mln/util/chrono.hpp>
#include <mln/util/constants.hpp>
#include <mln/util/image.hpp>
#include <mln/util/mat4.hpp>
#include <mln/util/projection.hpp>
#include <mln/util/run_loop.hpp>

#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace mln;

// Renderer::queryTerrainLatLng and Renderer::queryTerrainPixel: screen <-> LatLng on the terrain as
// drawn. A tap reads the ground under the finger; a callout or a scrubber dot is placed over a
// trail point on the terrain.
//
// The terrain is analytic, laid out in kilometres from the map's centre, so it is the same at
// every latitude: an east-west ridge 3 km north of the centre, 600 m above a 1,000 m floor, and a
// gentle undulation of 120 m everywhere. Each DEM pixel holds the height at its centre, where the
// samplers place it, so the drawn surface is the formula up to the DEM's 0.1 m steps and bilinear
// interpolation.

namespace {

constexpr double kmPerDegree = 111.32;
constexpr uint32_t viewWidth = 390;
constexpr uint32_t viewHeight = 844;
constexpr double ridgeNorthKm = 3.0;

double hillsMeters(double northKm, double eastKm) {
    return 1000.0 + 600.0 * std::exp(-std::pow((northKm - ridgeNorthKm) / 1.2, 2.0)) +
           120.0 * std::sin(2.0 * M_PI * eastKm / 5.0) * std::cos(2.0 * M_PI * northKm / 7.0);
}

// `northKm`/`eastKm` from `origin`, on a sphere's local tangent (the formula's own coordinates).
LatLng offset(const LatLng& origin, double northKm, double eastKm) {
    return {origin.latitude() + northKm / kmPerDegree,
            origin.longitude() + eastKm / (kmPerDegree * std::cos(util::deg2rad(origin.latitude())))};
}

double hillsMeters(const LatLng& origin, const LatLng& point) {
    const double northKm = (point.latitude() - origin.latitude()) * kmPerDegree;
    // The shorter way round, so the hills run on across the antimeridian.
    double eastDegrees = point.longitude() - origin.longitude();
    eastDegrees -= 360.0 * std::round(eastDegrees / 360.0);
    const double eastKm = eastDegrees * kmPerDegree * std::cos(util::deg2rad(origin.latitude()));
    return hillsMeters(northKm, eastKm);
}

std::string hillsTile(const LatLng& origin, const std::string& url) {
    int z = 0;
    int x = 0;
    int y = 0;
    const auto name = url.substr(url.rfind('/') + 1);
    std::sscanf(name.c_str(), "%d-%d-%d.png", &z, &x, &y);
    constexpr uint32_t size = 256;
    const double n = std::pow(2.0, z);
    PremultipliedImage image({size, size});
    for (uint32_t row = 0; row < size; ++row) {
        const double mercatorY = (y + (row + 0.5) / size) / n;
        const double latitude = util::rad2deg(std::atan(std::sinh(M_PI * (1.0 - 2.0 * mercatorY))));
        for (uint32_t col = 0; col < size; ++col) {
            const double longitude = (x + (col + 0.5) / size) / n * 360.0 - 180.0;
            // Mapbox Terrain-RGB: -10000 + (R * 65536 + G * 256 + B) * 0.1.
            const auto value = static_cast<uint32_t>(
                std::lround((hillsMeters(origin, {latitude, longitude}) + 10000.0) * 10.0));
            uint8_t* px = image.data.get() + (static_cast<size_t>(row) * size + col) * 4;
            px[0] = static_cast<uint8_t>(value >> 16);
            px[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            px[2] = static_cast<uint8_t>(value & 0xFF);
            px[3] = 255;
        }
    }
    return encodePNG(image);
}

/// `demZoom`: serve the DEM at that zoom only (minzoom = maxzoom), so no coarser tile stands in
/// where one is missing; by default up to z11.
std::string hillsStyle(std::optional<double> exaggeration, std::optional<int> demZoom = std::nullopt) {
    const std::string terrain = exaggeration ? R"("terrain": {"source": "dem", "exaggeration": )" +
                                                   std::to_string(*exaggeration) + "},"
                                             : "";
    return R"STYLE({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"],
      "encoding": "mapbox",
      "minzoom": )STYLE" +
           std::to_string(demZoom.value_or(0)) + R"STYLE(,
      "maxzoom": )STYLE" +
           std::to_string(demZoom.value_or(11)) + R"STYLE(,
      "tileSize": 256
    },
    "patch": {"type": "geojson", "data": {"type": "FeatureCollection", "features": []}}
  },
  )STYLE" + terrain +
           R"STYLE(
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "white"}},
    {"id": "patch", "type": "fill", "source": "patch", "paint": {"fill-color": "#f00"}}
  ]
})STYLE";
}

struct CameraCase {
    LatLng center;
    double pitch;
    double bearing;
};

std::string describe(const CameraCase& c) {
    return "centre " + std::to_string(c.center.latitude()) + ", pitch " + std::to_string(c.pitch) + ", bearing " +
           std::to_string(c.bearing);
}

// A phone-sized still map over the hills, with the centre at the ground's drawn height.
struct HillsScene {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    HeadlessFrontend frontend{{viewWidth, viewHeight}, 1};
    MapAdapter map{frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())};
    CameraCase camera;
    std::optional<double> exaggeration;
    PremultipliedImage image;

    /// A DEM tile `withheld` answers with no content, leaving a hole no DEM covers.
    using Withheld = std::function<bool(const Resource::TileData&)>;

    explicit HillsScene(const CameraCase& camera_,
                        std::optional<double> exaggeration_ = 1.5,
                        std::optional<int> demZoom = std::nullopt,
                        Withheld withheld = {})
        : camera(camera_),
          exaggeration(exaggeration_) {
        const LatLng origin = camera.center;
        fileSource->tileResponse = [origin, withheld](const Resource& resource) {
            Response res;
            if (withheld && resource.tileData && withheld(*resource.tileData)) {
                res.noContent = true;
                return res;
            }
            // Keyed by the whole origin: the hills are laid out from it, so two scenes at the
            // same latitude but different longitudes ask for the same tiles with different hills.
            static std::unordered_map<std::string, std::string> cache;
            const std::string key = std::to_string(origin.latitude()) + "," + std::to_string(origin.longitude()) +
                                    resource.url;
            auto it = cache.find(key);
            if (it == cache.end()) {
                it = cache.emplace(key, hillsTile(origin, resource.url)).first;
            }
            res.data = std::make_shared<std::string>(it->second);
            return res;
        };
        map.getStyle().loadJSON(hillsStyle(exaggeration, demZoom));
        map.setBounds(BoundOptions().withMaxPitch(85.0));
        map.jumpTo(CameraOptions()
                       .withCenter(camera.center)
                       .withZoom(13.0)
                       .withPitch(camera.pitch)
                       .withBearing(camera.bearing)
                       .withCenterAltitude(hillsMeters(0.0, 0.0) * exaggeration.value_or(0.0)));
        render();
    }

    void render() { image = frontend.render(map).image; }

    Renderer& renderer() { return *frontend.getRenderer(); }

    TransformState state() const { return *frontend.getTransformState(); }

    // Where a point `meters` above sea level is drawn, top-left origin: the projection the
    // renderer draws the terrain with, which takes heights in metres. Independent of the API.
    ScreenCoordinate project(const LatLng& location, double meters) const {
        const TransformState s = state();
        // In the world copy nearest the centre, which may itself lie turns round the world.
        const double longitude = location.wrapped().longitude();
        const double turns = std::round((s.getLatLng(LatLng::Unwrapped).longitude() - longitude) / 360.0);
        const LatLng copy{location.latitude(), longitude + 360.0 * turns};
        const Point<double> world = Projection::project(copy, s.getScale());
        vec4 clip;
        matrix::transformMat4(clip, vec4{{world.x, world.y, meters, 1.0}}, s.getProjectionMatrix());
        const Size size = s.getSize();
        return {(clip[0] / clip[3] + 1.0) * size.width / 2.0, (1.0 - clip[1] / clip[3]) * size.height / 2.0};
    }

    // A square `halfMeters` either side of `location`, draped on the terrain.
    void setPatch(const LatLng& location, double halfMeters) {
        const double half = halfMeters / 1000.0;
        const LatLng sw = offset(location, -half, -half);
        const LatLng ne = offset(location, half, half);
        Polygon<double> square{{{sw.longitude(), sw.latitude()},
                                {ne.longitude(), sw.latitude()},
                                {ne.longitude(), ne.latitude()},
                                {sw.longitude(), ne.latitude()},
                                {sw.longitude(), sw.latitude()}}};
        map.getStyle().getSource("patch")->as<style::GeoJSONSource>()->setGeoJSON(Geometry<double>{square});
    }

    // A circle marker of `radius` px at `location`, as the app draws trail markers and the
    // elevation scrubber's dot: red, no stroke, aligned to the viewport or to the map.
    void setMarker(const LatLng& location, float radius, style::AlignmentType alignment) {
        auto& style = map.getStyle();
        if (!style.getSource("marker")) {
            style.addSource(std::make_unique<style::GeoJSONSource>("marker"));
        }
        style.getSource("marker")->as<style::GeoJSONSource>()->setGeoJSON(
            Geometry<double>{Point<double>{location.longitude(), location.latitude()}});
        if (style.getLayer("marker")) {
            style.removeLayer("marker");
        }
        auto layer = std::make_unique<style::CircleLayer>("marker", "marker");
        layer->setCircleColor(Color::red());
        layer->setCircleRadius(radius);
        layer->setCirclePitchAlignment(alignment);
        style.addLayer(std::move(layer));
    }

    // A flat red roof `halfKm` either side of `location`, `meters` above the ground at its centre:
    // an extrusion with its base at its height, so it has no walls, only the roof.
    void setRoof(const LatLng& location, double halfKm, float meters) {
        auto& style = map.getStyle();
        if (!style.getSource("roof")) {
            style.addSource(std::make_unique<style::GeoJSONSource>("roof"));
            auto layer = std::make_unique<style::FillExtrusionLayer>("roof", "roof");
            layer->setFillExtrusionColor(Color::red());
            layer->setFillExtrusionBase(meters);
            layer->setFillExtrusionHeight(meters);
            style.addLayer(std::move(layer));
        }
        const LatLng sw = offset(location, -halfKm, -halfKm);
        const LatLng ne = offset(location, halfKm, halfKm);
        style.getSource("roof")->as<style::GeoJSONSource>()->setGeoJSON(
            Geometry<double>{Polygon<double>{{{sw.longitude(), sw.latitude()},
                                              {ne.longitude(), sw.latitude()},
                                              {ne.longitude(), ne.latitude()},
                                              {sw.longitude(), ne.latitude()},
                                              {sw.longitude(), sw.latitude()}}}});
    }

    // Where that roof is drawn on the ground drawn at its centre: the centre of area of its four
    // corners projected at the drawn height there plus `meters`.
    ScreenCoordinate roofCentre(const LatLng& location, double halfKm, double meters) {
        const double height = renderer().queryTerrainElevation(location).value_or(0.0) + meters;
        std::array<ScreenCoordinate, 4> q;
        const std::array<std::pair<double, double>, 4> corners{
            {{-halfKm, -halfKm}, {-halfKm, halfKm}, {halfKm, halfKm}, {halfKm, -halfKm}}};
        for (size_t i = 0; i < 4; ++i) {
            q[i] = project(offset(location, corners[i].first, corners[i].second), height);
        }
        double area = 0.0;
        double x = 0.0;
        double y = 0.0;
        for (size_t i = 0; i < 4; ++i) {
            const auto& a = q[i];
            const auto& b = q[(i + 1) % 4];
            const double cross = a.x * b.y - b.x * a.y;
            area += cross;
            x += (a.x + b.x) * cross;
            y += (a.y + b.y) * cross;
        }
        return {x / (3.0 * area), y / (3.0 * area)};
    }

    // The centre of every red pixel on the screen (the roof is drawn without antialiasing).
    ScreenCoordinate redCentre() const {
        double x = 0.0;
        double y = 0.0;
        int count = 0;
        for (uint32_t row = 0; row < image.size.height; ++row) {
            for (uint32_t col = 0; col < image.size.width; ++col) {
                const uint8_t* px = image.data.get() + (static_cast<size_t>(row) * image.size.width + col) * 4;
                if (px[0] > 128 && px[1] < 128) {
                    x += col + 0.5;
                    y += row + 0.5;
                    ++count;
                }
            }
        }
        return count ? ScreenCoordinate{x / count, y / count} : ScreenCoordinate{-1.0, -1.0};
    }

    // The centre of the patch drawn within `radius` of `around`, in screen coordinates (a pixel's
    // centre is half a pixel in), weighted by how much of each pixel it covers: red over the white
    // ground, so its green channel falls from 255 to 0 as its coverage rises from 0 to 1. Also
    // how many pixels it covers fully.
    std::pair<ScreenCoordinate, int> redCentroid(const ScreenCoordinate& around, int radius = 16) const {
        double sumX = 0.0;
        double sumY = 0.0;
        double sumWeight = 0.0;
        int count = 0;
        const int cx = static_cast<int>(std::lround(around.x));
        const int cy = static_cast<int>(std::lround(around.y));
        for (int y = cy - radius; y <= cy + radius; ++y) {
            for (int x = cx - radius; x <= cx + radius; ++x) {
                if (x < 0 || y < 0 || std::cmp_greater_equal(x, image.size.width) ||
                    std::cmp_greater_equal(y, image.size.height)) {
                    continue;
                }
                const uint8_t* px = image.data.get() + (static_cast<size_t>(y) * image.size.width + x) * 4;
                if (px[0] < 200 || std::abs(px[1] - px[2]) > 20) {
                    continue; // neither red nor a blend of red and white
                }
                const double weight = (255.0 - px[1]) / 255.0;
                sumX += weight * (x + 0.5);
                sumY += weight * (y + 0.5);
                sumWeight += weight;
                count += px[1] < 100 ? 1 : 0;
            }
        }
        if (sumWeight == 0.0) {
            return {{}, 0};
        }
        return {{sumX / sumWeight, sumY / sumWeight}, count};
    }
};

double distance(const ScreenCoordinate& a, const ScreenCoordinate& b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

// A grid over the phone screen, from the top (sky at high pitch) to the bottom.
std::vector<ScreenCoordinate> screenGrid() {
    std::vector<ScreenCoordinate> points;
    for (const double y : {40.0, 150.0, 260.0, 370.0, 480.0, 590.0, 700.0, 810.0}) {
        for (const double x : {15.0, 105.0, 195.0, 285.0, 375.0}) {
            points.emplace_back(x, y);
        }
    }
    return points;
}

std::vector<CameraCase> cameraCases() {
    std::vector<CameraCase> cases;
    for (const LatLng center : {LatLng{0.3, 10.0}, LatLng{46.0, 10.0}, LatLng{66.0, -150.0}}) {
        for (const double pitch : {0.0, 45.0, 60.0, 75.0}) {
            for (const double bearing : {0.0, 90.0, 217.0}) {
                cases.push_back({.center = center, .pitch = pitch, .bearing = bearing});
            }
        }
    }
    return cases;
}

} // namespace

// A tap reads the ground drawn under the finger, and that ground is drawn back at the finger: the
// LatLng sits at the drawn height on the finger's line of sight, so projecting it at its drawn
// height (queryTerrainElevation) lands on the finger, and queryTerrainPixel says so, unoccluded.
// At sea level (today's latLngForPixel) the ground under the finger is hundreds of metres off.
TEST(TerrainScreenQuery, PixelToLatLngToPixelRoundTrips) {
    for (const auto& camera : cameraCases()) {
        SCOPED_TRACE(describe(camera));
        HillsScene scene(camera);
        int hits = 0;
        for (const auto& finger : screenGrid()) {
            SCOPED_TRACE(::testing::Message() << "finger (" << finger.x << ", " << finger.y << ")");
            const auto ground = scene.renderer().queryTerrainLatLng(finger);
            if (!ground) {
                // Only the sky has no ground: at pitch 60 and under, every finger meets it.
                EXPECT_GE(camera.pitch, 75.0) << "no ground under the finger";
                continue;
            }
            ++hits;
            const auto drawnMeters = scene.renderer().queryTerrainElevation(*ground);
            ASSERT_TRUE(drawnMeters) << "no DEM under the ground the finger hit";
            const double drawn = distance(scene.project(*ground, *drawnMeters), finger);
            EXPECT_LT(drawn, 0.5) << "the ground under the finger is not drawn under it";

            const auto pixel = scene.renderer().queryTerrainPixel(*ground);
            ASSERT_TRUE(pixel) << "the ground under the finger has no pixel";
            EXPECT_FALSE(pixel->occluded) << "the ground the finger sees is reported hidden";
            const double roundTrip = distance(pixel->point, finger);
            EXPECT_LT(roundTrip, 0.5) << "the ground under the finger maps back elsewhere";
        }
        // At pitch 75 the top rows are sky; everything else is ground.
        EXPECT_GE(hits, camera.pitch >= 75.0 ? 25 : 40);
    }
}

// Looking east across the antimeridian: the ground beyond it reads as a wrapped longitude (as
// latLngForPixel answers), and that longitude maps back to the finger, on the terrain drawn there.
TEST(TerrainScreenQuery, RoundTripsAcrossTheAntimeridian) {
    const CameraCase camera{.center = {0.3, 179.99}, .pitch = 60.0, .bearing = 90.0};
    HillsScene scene(camera);
    int east = 0;
    int west = 0;
    for (const auto& finger : screenGrid()) {
        SCOPED_TRACE(::testing::Message() << "finger (" << finger.x << ", " << finger.y << ")");
        const auto ground = scene.renderer().queryTerrainLatLng(finger);
        ASSERT_TRUE(ground);
        EXPECT_GE(ground->longitude(), -180.0);
        EXPECT_LE(ground->longitude(), 180.0);
        (ground->longitude() < 0.0 ? east : west) += 1;
        const auto pixel = scene.renderer().queryTerrainPixel(*ground);
        ASSERT_TRUE(pixel);
        EXPECT_FALSE(pixel->occluded);
        EXPECT_LT(distance(pixel->point, finger), 0.5) << "the ground under the finger maps back elsewhere";
    }
    // Both sides of the antimeridian are on the screen.
    EXPECT_GT(east, 5);
    EXPECT_GT(west, 5);
}

// Tap -> LatLng -> elevation, the "what is the ground here?" flow: beyond the antimeridian the tap
// reads a wrapped longitude, and the height there is the height drawn there.
TEST(TerrainScreenQuery, TheTappedGroundHasItsHeightAcrossTheAntimeridian) {
    const CameraCase camera{.center = {0.3, 179.99}, .pitch = 60.0, .bearing = 90.0};
    HillsScene scene(camera);
    int east = 0;
    for (const auto& finger : screenGrid()) {
        SCOPED_TRACE(::testing::Message() << "finger (" << finger.x << ", " << finger.y << ")");
        const auto ground = scene.renderer().queryTerrainLatLng(finger);
        ASSERT_TRUE(ground);
        east += ground->longitude() < 0.0 ? 1 : 0;
        const auto meters = scene.renderer().queryTerrainElevation(*ground);
        ASSERT_TRUE(meters) << "no height at " << ground->longitude();
        // The DEM holds the hills to its 0.1 m steps, and bilinear interpolation between its
        // pixels departs from them by well under a metre on these slopes.
        EXPECT_NEAR(*meters, hillsMeters(camera.center, *ground) * 1.5, 1.0);
    }
    EXPECT_GT(east, 5) << "no ground beyond the antimeridian";
}

// LatLngBounds can hold the camera's centre a turn or more round the world (541 E here), and the
// frame draws the world copy there. A longitude given in [-180, 180] is read, and drawn, in that
// copy: the tap round trips, and the tapped ground has its height.
TEST(TerrainScreenQuery, ACentreTurnsRoundTheWorldUsesTheCopyItDraws) {
    for (const std::optional<double> exaggeration : {std::optional<double>{1.5}, std::optional<double>{}}) {
        SCOPED_TRACE(exaggeration ? "terrain" : "no terrain");
        HillsScene scene({.center = {0.3, 179.0}, .pitch = 60.0, .bearing = 90.0}, exaggeration);
        scene.map.setBounds(
            BoundOptions().withMaxPitch(85.0).withLatLngBounds(LatLngBounds::hull({-10.0, 530.0}, {10.0, 560.0})));
        scene.map.jumpTo(CameraOptions().withCenter(LatLng{0.3, 541.0}));
        scene.render();
        ASSERT_EQ(scene.state().getLatLng(LatLng::Unwrapped).longitude(), 541.0);
        int east = 0;
        for (const auto& finger : screenGrid()) {
            SCOPED_TRACE(::testing::Message() << "finger (" << finger.x << ", " << finger.y << ")");
            const auto ground = scene.renderer().queryTerrainLatLng(finger);
            ASSERT_TRUE(ground);
            east += ground->longitude() < 0.0 ? 1 : 0;
            const auto pixel = scene.renderer().queryTerrainPixel(*ground);
            if (!pixel) {
                ADD_FAILURE() << "no pixel for the ground at " << ground->longitude();
                continue;
            }
            EXPECT_LT(distance(pixel->point, finger), 0.5);
            const double meters = exaggeration ? scene.renderer().queryTerrainElevation(*ground).value_or(-1e9) : 0.0;
            EXPECT_LT(distance(scene.project(*ground, meters), finger), 0.5) << "no height, or not drawn there";
        }
        EXPECT_GT(east, 5) << "no ground beyond the antimeridian";
    }
}

// A patch of ground marked at the LatLng the tap returned is drawn under the finger: a pixel proof
// that the answer is the ground the GPU draws (a fill draped on the terrain mesh), not only the
// CPU's DEM. Today's sea-level answer marks ground tens of pixels away.
TEST(TerrainScreenQuery, GroundMarkedAtTheTappedLatLngIsDrawnUnderTheFinger) {
    for (const auto& camera : {CameraCase{.center = {46.0, 10.0}, .pitch = 60.0, .bearing = 0.0},
                               CameraCase{.center = {46.0, 10.0}, .pitch = 75.0, .bearing = 217.0},
                               CameraCase{.center = {66.0, -150.0}, .pitch = 45.0, .bearing = 90.0}}) {
        HillsScene scene(camera);
        for (const auto& finger :
             {ScreenCoordinate{195.0, 600.0}, ScreenCoordinate{80.0, 420.0}, ScreenCoordinate{330.0, 750.0}}) {
            SCOPED_TRACE(::testing::Message()
                         << describe(camera) << ", finger (" << finger.x << ", " << finger.y << ")");
            const auto ground = scene.renderer().queryTerrainLatLng(finger);
            ASSERT_TRUE(ground);
            scene.setPatch(*ground, 25.0);
            // On OpenGL the drape shows the source's new data a frame late (llvmpipe; see
            // TerrainLineDrape.RedrawnDrapeUsesTheFramesPaintParams), so allow one more frame.
            scene.render();
            if (scene.redCentroid(finger, 24).second == 0) {
                scene.render();
            }
            const auto [centre, count] = scene.redCentroid(finger, 24);
            ASSERT_GT(count, 2) << "no patch drawn near the finger";
            EXPECT_LT(distance(centre, finger), 0.5)
                << "the patch is drawn at (" << centre.x << ", " << centre.y << "), not under the finger";
        }
    }
}

// A point behind the ridge, seen from the south at a low angle, is drawn where the ridge hides it:
// its pixel is reported occluded, and a tap there reads the ridge, not the point. A point on the
// near face of the ridge is not.
TEST(TerrainScreenQuery, APointBehindARidgeIsOccluded) {
    const CameraCase camera{.center = {46.0, 10.0}, .pitch = 75.0, .bearing = 0.0};
    HillsScene scene(camera);

    const LatLng hidden = offset(camera.center, 6.0, 0.0);
    const auto pixel = scene.renderer().queryTerrainPixel(hidden);
    ASSERT_TRUE(pixel);
    EXPECT_GT(pixel->point.y, 0.0);
    EXPECT_LT(pixel->point.y, viewHeight);
    const auto meters = scene.renderer().queryTerrainElevation(hidden);
    ASSERT_TRUE(meters);
    EXPECT_LT(distance(pixel->point, scene.project(hidden, *meters)), 1e-6);
    EXPECT_TRUE(pixel->occluded) << "a point behind the ridge is reported visible";
    const auto seen = scene.renderer().queryTerrainLatLng(pixel->point);
    ASSERT_TRUE(seen);
    const double seenNorthKm = (seen->latitude() - camera.center.latitude()) * kmPerDegree;
    EXPECT_NEAR(seenNorthKm, ridgeNorthKm, 1.0) << "the tap does not read the ridge in front";

    const LatLng nearFace = offset(camera.center, 2.0, 0.0);
    const auto facePixel = scene.renderer().queryTerrainPixel(nearFace);
    ASSERT_TRUE(facePixel);
    EXPECT_FALSE(facePixel->occluded) << "a point on the ridge's near face is reported hidden";
}

// Occlusion counts only ground the loaded DEM holds: with the DEM tiles under the ridge missing
// (no coarser tile stands in), nothing loaded lies in front of the point behind it, so it is not
// occluded, and a tap at its pixel reads the point itself. With those tiles loaded, the same point
// from the same camera is occluded.
TEST(TerrainScreenQuery, GroundNoLoadedDEMCoversNeverOccludes) {
    // Pitch 60: at 75 a still render never finishes with the DEM held to one zoom.
    const CameraCase camera{.center = {46.0, 10.0}, .pitch = 60.0, .bearing = 0.0};
    constexpr int demZoom = 13;
    const auto row = [](const LatLng& latLng) {
        return static_cast<int64_t>(std::floor(Projection::project(latLng, 1.0).y / util::tileSize_D * (1 << demZoom)));
    };
    // The DEM tile row that holds the ridge's crest and both its flanks.
    const int64_t ridgeRow = row(offset(camera.center, ridgeNorthKm, 0.0));
    ASSERT_EQ(row(offset(camera.center, 1.5, 0.0)), ridgeRow);
    ASSERT_EQ(row(offset(camera.center, 4.5, 0.0)), ridgeRow);
    const LatLng hidden = offset(camera.center, 5.0, 0.0);
    ASSERT_NE(row(hidden), ridgeRow);

    for (const bool hole : {false, true}) {
        SCOPED_TRACE(hole ? "ridge tiles missing" : "ridge tiles loaded");
        HillsScene scene(
            camera, 1.5, demZoom, [&](const Resource::TileData& tile) { return hole && tile.y == ridgeRow; });
        ASSERT_EQ(scene.renderer().queryTerrainElevation(offset(camera.center, ridgeNorthKm, 0.0)).has_value(), !hole);
        ASSERT_TRUE(scene.renderer().queryTerrainElevation(hidden)) << "no DEM under the point";
        const auto pixel = scene.renderer().queryTerrainPixel(hidden);
        ASSERT_TRUE(pixel);
        EXPECT_GT(pixel->point.y, 0.0);
        EXPECT_LT(pixel->point.y, viewHeight);
        EXPECT_EQ(pixel->occluded, !hole);
        const auto seen = scene.renderer().queryTerrainLatLng(pixel->point);
        ASSERT_TRUE(seen);
        const double seenNorthKm = (seen->latitude() - camera.center.latitude()) * kmPerDegree;
        EXPECT_NEAR(seenNorthKm, hole ? 5.0 : ridgeNorthKm, hole ? 0.01 : 1.0);
    }
}

// Above the horizon there is no ground to read.
TEST(TerrainScreenQuery, TheSkyHasNoGround) {
    const CameraCase camera{.center = {46.0, 10.0}, .pitch = 75.0, .bearing = 0.0};
    HillsScene scene(camera);
    for (const auto& sky : {ScreenCoordinate{195.0, 5.0}, ScreenCoordinate{10.0, 20.0}}) {
        EXPECT_FALSE(scene.renderer().queryTerrainLatLng(sky)) << "ground read at (" << sky.x << ", " << sky.y << ")";
    }
    // Nor is there any at no point at all.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(scene.renderer().queryTerrainLatLng({nan, 400.0}));
    EXPECT_FALSE(scene.renderer().queryTerrainLatLng({195.0, std::numeric_limits<double>::infinity()}));
}

// Without terrain both queries are the 2D map's, bit for bit, so callers need one code path; a
// point behind the camera, where the 2D projection mirrors it, has no pixel.
TEST(TerrainScreenQuery, WithoutTerrainTheAnswersAreTheMapsAtSeaLevel) {
    for (const auto& camera : {CameraCase{.center = {46.0, 10.0}, .pitch = 60.0, .bearing = 217.0},
                               CameraCase{.center = {0.3, 179.99}, .pitch = 30.0, .bearing = 90.0}}) {
        SCOPED_TRACE(describe(camera));
        HillsScene scene(camera, std::nullopt);
        for (const auto& finger : screenGrid()) {
            SCOPED_TRACE(::testing::Message() << "finger (" << finger.x << ", " << finger.y << ")");
            const auto ground = scene.renderer().queryTerrainLatLng(finger);
            ASSERT_TRUE(ground);
            const LatLng seaLevel = scene.map.latLngForPixel(finger);
            EXPECT_EQ(ground->latitude(), seaLevel.latitude());
            EXPECT_EQ(ground->longitude(), seaLevel.longitude());

            // The longitude given on the far side of the antimeridian lands on the same pixel.
            for (const double turn : {0.0, 360.0, -360.0}) {
                const LatLng point{seaLevel.latitude(), seaLevel.longitude() + turn};
                const auto pixel = scene.renderer().queryTerrainPixel(point);
                ASSERT_TRUE(pixel);
                const ScreenCoordinate expected = scene.map.pixelForLatLng(point);
                EXPECT_EQ(pixel->point.x, expected.x);
                EXPECT_EQ(pixel->point.y, expected.y);
                EXPECT_FALSE(pixel->occluded);
            }
        }
    }

    const CameraCase camera{.center = {46.0, 10.0}, .pitch = 75.0, .bearing = 0.0};
    HillsScene scene(camera, std::nullopt);
    const LatLng behind = offset(camera.center, -40.0, 0.0);
    EXPECT_FALSE(scene.renderer().queryTerrainPixel(behind)) << "a point behind the camera has a pixel";
}

// With terrain, a point behind the camera has no pixel either, even over loaded DEM.
TEST(TerrainScreenQuery, APointBehindTheCameraHasNoPixel) {
    // At 46.05 N the ground under the eye is in the DEM tile the bottom of the screen loads.
    const CameraCase camera{.center = {46.05, 10.0}, .pitch = 75.0, .bearing = 0.0};
    HillsScene scene(camera);
    const TransformState state = scene.state();
    // Behind the eye, on its meridian: 1.5 km, past where the plane through the eye square to the
    // line of sight meets the ground (0.6 km behind it at this pitch).
    const auto eye = state.getFreeCameraOptions().getLocation();
    ASSERT_TRUE(eye);
    const LatLng behind = offset(eye->location, -1.5, 0.0);
    ASSERT_TRUE(scene.renderer().queryTerrainElevation(behind)) << "no DEM behind the camera; move the point";
    EXPECT_FALSE(scene.renderer().queryTerrainPixel(behind)) << "a point behind the camera has a pixel";
}

// Before any DEM tile has loaded the terrain's height is unknown, so neither query answers; nor
// before the first frame, when there is no transform to read.
TEST(TerrainScreenQuery, NoAnswerWithoutDEMOrBeforeTheFirstFrame) {
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    // The DEM never arrives.
    fileSource->tileResponse = [](const Resource&) -> std::optional<Response> {
        return std::nullopt;
    };
    HeadlessFrontend frontend{{viewWidth, viewHeight}, 1};
    MapAdapter map{frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Continuous).withSize(frontend.getSize())};
    map.getStyle().loadJSON(hillsStyle(1.5));
    const LatLng center{46.0, 10.0};
    map.jumpTo(CameraOptions().withCenter(center).withZoom(13.0).withPitch(60.0));

    Renderer& renderer = *frontend.getRenderer();
    EXPECT_FALSE(renderer.queryTerrainLatLng({195.0, 600.0})) << "ground read before the first frame";
    EXPECT_FALSE(renderer.queryTerrainPixel(center)) << "a pixel before the first frame";

    for (int i = 0; i < 5; ++i) {
        loop.runOnce();
        frontend.renderFrame();
    }
    ASSERT_TRUE(frontend.getTransformState()) << "no frame was rendered";
    EXPECT_FALSE(renderer.queryTerrainLatLng({195.0, 600.0})) << "ground read with no DEM loaded";
    EXPECT_FALSE(renderer.queryTerrainPixel(center)) << "a pixel with no DEM loaded";
}

// A circle marker is drawn on the ground at its LatLng, where queryTerrainPixel puts it and a
// fill draped there is drawn, from any camera: pitched, centred on it or not, viewport- or
// map-aligned, at the trail marker's and scrubber dot's radii.
//
// The point lies a fifth of a z13 tile from its tile's east edge, so the tile buffer of its eastern
// neighbour holds it too. A still render keeps buffer points (circles are not clipped to their
// tile), and with terrain the neighbour read the point's height off its own DEM beyond its west
// edge: a second circle, metres off the ground, whenever the neighbour is on screen.
TEST(TerrainScreenQuery, ACircleIsDrawnOnTheGroundAtItsLatLng) {
    const LatLng origin{46.0, 10.0};
    const LatLng point{46.000197, 10.011712};
    struct View {
        const char* name;
        LatLng hillsOrigin; // where the hills are laid out from, and the scene's first centre
        bool centred;       // then jump to centre the camera on the point
    };
    for (const View& view : {View{.name = "off-centre", .hillsOrigin = origin, .centred = false},
                             View{.name = "centred", .hillsOrigin = origin, .centred = true},
                             View{.name = "centred, hills laid out from it", .hillsOrigin = point, .centred = true}}) {
        for (const double pitch : {0.0, 60.0, 75.0}) {
            HillsScene scene({.center = view.hillsOrigin, .pitch = pitch, .bearing = 0.0});
            if (view.centred) {
                scene.map.jumpTo(CameraOptions().withCenter(point).withCenterAltitude(
                    hillsMeters(view.hillsOrigin, point) * *scene.exaggeration));
            }
            for (const auto alignment : {style::AlignmentType::Viewport, style::AlignmentType::Map}) {
                for (const float radius : {4.0f, 10.0f}) {
                    SCOPED_TRACE(::testing::Message() << view.name << ", pitch " << pitch << ", "
                                                      << (alignment == style::AlignmentType::Map ? "map" : "viewport")
                                                      << "-aligned, radius " << radius);
                    scene.setMarker(point, radius, alignment);
                    scene.render();
                    const auto pixel = scene.renderer().queryTerrainPixel(point);
                    ASSERT_TRUE(pixel);
                    ASSERT_FALSE(pixel->occluded);
                    const auto [centre, count] = scene.redCentroid(pixel->point, 30);
                    ASSERT_GT(count, 2) << "no circle drawn near the point";
                    EXPECT_LT(distance(centre, pixel->point), 0.3)
                        << "the circle is drawn at (" << centre.x << ", " << centre.y << "), the ground at ("
                        << pixel->point.x << ", " << pixel->point.y << ")";
                }
            }
        }
    }
}

// The same from each side: a point a fifth of a tile in from an edge of its z13 tile, so the
// neighbour across that edge holds it in its buffer, beyond its west, east, south or north edge.
// Each such edge is also an edge of the z11 DEM tiles (the DEM's maxzoom), so the neighbour's own
// DEM ends there: within one DEM tile the neighbour read the right height.
TEST(TerrainScreenQuery, ACircleNearAnyEdgeOfItsTileIsDrawnOnTheGround) {
    const LatLng origin{46.0, 10.0};
    constexpr int zoom = 13;
    constexpr int demZoom = 11;
    constexpr int perDEMTile = 1 << (zoom - demZoom);
    const Point<double> originTile = Projection::project(origin, 1.0) / util::tileSize_D * double(1 << zoom);
    // The z13 tiles at the south-east and north-west corners of the origin's z11 DEM tile.
    const double west = std::floor(originTile.x / perDEMTile) * perDEMTile;
    const double north = std::floor(originTile.y / perDEMTile) * perDEMTile;
    const Point<double> southEast{west + perDEMTile - 1, north + perDEMTile - 1};
    const Point<double> northWest{west, north};
    for (const auto& [side, tile, inTile] : {std::tuple{"east", southEast, Point<double>{0.8, 0.5}},
                                             std::tuple{"south", southEast, Point<double>{0.5, 0.8}},
                                             std::tuple{"west", northWest, Point<double>{0.2, 0.5}},
                                             std::tuple{"north", northWest, Point<double>{0.5, 0.2}}}) {
        SCOPED_TRACE(side);
        const LatLng point = Projection::unproject((tile + inTile) * util::tileSize_D / double(1 << zoom), 1.0);
        HillsScene scene({.center = origin, .pitch = 60.0, .bearing = 0.0});
        scene.map.jumpTo(
            CameraOptions().withCenter(point).withCenterAltitude(hillsMeters(origin, point) * *scene.exaggeration));
        scene.setMarker(point, 6.0f, style::AlignmentType::Viewport);
        scene.render();
        const auto pixel = scene.renderer().queryTerrainPixel(point);
        ASSERT_TRUE(pixel);
        const auto [centre, count] = scene.redCentroid(pixel->point, 30);
        ASSERT_GT(count, 2) << "no circle drawn near the point";
        EXPECT_LT(distance(centre, pixel->point), 0.3)
            << "the circle is drawn at (" << centre.x << ", " << centre.y << "), the ground at (" << pixel->point.x
            << ", " << pixel->point.y << ")";
    }
}

// An extrusion stands on the ground at the centre of its footprint: the whole roof is raised by the
// height drawn there. The footprint lies inside its tile, clear of every neighbour's buffer.
TEST(TerrainScreenQuery, AnExtrusionIsRaisedByTheGroundAtTheCentreOfItsFootprint) {
    const LatLng origin{46.0, 10.0};
    constexpr double halfKm = 0.25;
    constexpr float meters = 80.0f; // clear of the ground on these slopes, so none of it is hidden
    for (const double pitch : {0.0, 60.0, 75.0}) {
        SCOPED_TRACE(::testing::Message() << "pitch " << pitch);
        HillsScene scene({.center = origin, .pitch = pitch, .bearing = 0.0});
        scene.setRoof(origin, halfKm, meters);
        scene.render();
        const ScreenCoordinate expected = scene.roofCentre(origin, halfKm, meters);
        const ScreenCoordinate drawn = scene.redCentre();
        EXPECT_LT(distance(drawn, expected), 0.4) << "the roof is drawn at (" << drawn.x << ", " << drawn.y
                                                  << "), on the ground at (" << expected.x << ", " << expected.y << ")";
    }
}
