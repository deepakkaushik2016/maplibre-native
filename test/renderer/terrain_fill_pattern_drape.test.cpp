#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/map/transform_state.hpp>
#include <mln/style/image.hpp>
#include <mln/style/sources/geojson_source.hpp>
#include <mln/style/style.hpp>
#include <mln/util/geojson.hpp>
#include <mln/util/image.hpp>
#include <mln/util/mat4.hpp>
#include <mln/util/projection.hpp>
#include <mln/util/run_loop.hpp>

#include <chrono>
#include <cmath>
#include <exception>
#include <memory>
#include <string>

using namespace mln;

namespace {

// Flat ground under a fill-pattern layer: a polygon that covers everything west of the map center,
// from a GeoJSON source that stops at z10, drawn straight down at zoom 16 where the drape targets
// are far deeper than that tile. A solid red image is the pattern, so a pixel is red exactly where
// the polygon is.
const char* drapeStyle = R"STYLE({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"],
      "encoding": "mapbox",
      "maxzoom": 11,
      "tileSize": 256
    },
    "west": {
      "type": "geojson",
      "maxzoom": 10,
      "data": {
        "type": "Feature",
        "properties": {},
        "geometry": {
          "type": "Polygon",
          "coordinates": [[[9.98, 45.98], [10.0, 45.98], [10.0, 46.02], [9.98, 46.02], [9.98, 45.98]]]
        }
      }
    }
  },
  "terrain": {"source": "dem", "exaggeration": 1.0},
  "layers": [{"id": "west", "type": "fill", "source": "west",
              "paint": {"fill-pattern": "red", "fill-antialias": false}}]
})STYLE";

std::string demTile() {
    PremultipliedImage image({256, 256});
    for (uint32_t i = 0; i < 256 * 256; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        // Sea level in Terrain-RGB: (0 + 10000) / 0.1 = 100000 = 1 * 65536 + 134 * 256 + 160
        px[0] = 1;
        px[1] = 134;
        px[2] = 160;
        px[3] = 255;
    }
    return encodePNG(image);
}

std::unique_ptr<style::Image> redPattern() {
    PremultipliedImage image({8, 8});
    for (uint32_t i = 0; i < 8 * 8; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        px[0] = 255;
        px[1] = 0;
        px[2] = 0;
        px[3] = 255;
    }
    return std::make_unique<style::Image>("red", std::move(image), 1.0f);
}

} // namespace

// A fill-pattern drawable is placed into each drape target by the vertex shader from its own tile
// and the target's. The Metal pattern vertex stage did not place it: the whole tile was stretched
// over every target it touched, so the polygon showed as a thin strip near the middle instead of
// the west half of the view.
TEST(TerrainDrape, AFillPatternFromAShallowTileLandsWhereItsPolygonIs) {
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    const std::string dem = demTile();
    fileSource->tileResponse = [&](const Resource&) {
        Response res;
        res.data = std::make_shared<std::string>(dem);
        return res;
    };
    HeadlessFrontend frontend{{256, 256}, 1};
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()));
    map.getStyle().loadJSON(drapeStyle);
    map.getStyle().addImage(redPattern());
    map.jumpTo(CameraOptions().withCenter(LatLng{46.0, 10.0}).withZoom(16.0).withPitch(0.0));

    PremultipliedImage image;
    for (int i = 0; i < 4; ++i) {
        loop.runOnce();
        image = frontend.render(map).image;
    }

    const auto red = [&](uint32_t x, uint32_t y) {
        const uint8_t* px = image.data.get() + (y * image.size.width + x) * 4;
        return px[0] > 150 && px[1] < 90 && px[2] < 90;
    };
    int west = 0;
    int east = 0;
    for (uint32_t y = 96; y < 160; ++y) {
        for (uint32_t x = 16; x < 100; ++x) west += red(x, y) ? 1 : 0;
        for (uint32_t x = 156; x < 240; ++x) east += red(x, y) ? 1 : 0;
    }
    const int area = 64 * 84;
    EXPECT_GT(west, area * 9 / 10) << "east " << east;
    EXPECT_LT(east, area / 20) << "west " << west;
}

namespace {

// Flat ground 1,000 m up, a white background and a GeoJSON source that starts empty, drawn red.
const char* geojsonStyle = R"STYLE({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"],
      "encoding": "mapbox",
      "maxzoom": 11,
      "tileSize": 256
    },
    "patch": {"type": "geojson", "data": {"type": "FeatureCollection", "features": []}}
  },
  "terrain": {"source": "dem", "exaggeration": 1.0},
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "white"}},
    {"id": "patch", "type": "fill", "source": "patch", "paint": {"fill-color": "#f00"}}
  ]
})STYLE";

constexpr double groundMeters = 1000.0;

std::string flatDemTile(double meters) {
    // Terrain-RGB: height = -10000 + (R * 65536 + G * 256 + B) * 0.1
    const auto value = static_cast<uint32_t>(std::lround((meters + 10000.0) * 10.0));
    PremultipliedImage image({256, 256});
    for (uint32_t i = 0; i < 256 * 256; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        px[0] = static_cast<uint8_t>(value >> 16);
        px[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        px[2] = static_cast<uint8_t>(value & 0xFF);
        px[3] = 255;
    }
    return encodePNG(image);
}

} // namespace

// A GeoJSON change shows on the terrain in the first still frame after it, so a snapshot taken
// right after a feature is added has it. On OpenGL that frame drew no terrain at all: the frame
// re-renders the drape targets, whose fills leave the tile-clipping stencil test on, the camera
// is still so the terrain depth pass (which would reset it) is skipped, and the terrain mesh, a 3D
// drawable in a plain layer group, inherited that test and failed it against the main pass's
// cleared stencil.
TEST(TerrainDrape, AGeoJSONChangeShowsInTheFirstStillFrameAfterIt) {
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    const std::string dem = flatDemTile(groundMeters);
    fileSource->tileResponse = [&](const Resource&) {
        Response res;
        res.data = std::make_shared<std::string>(dem);
        return res;
    };
    HeadlessFrontend frontend{{256, 256}, 1};
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()));
    map.getStyle().loadJSON(geojsonStyle);
    const LatLng center{46.0, 10.0};
    map.jumpTo(CameraOptions().withCenter(center).withZoom(14.0).withPitch(45.0).withCenterAltitude(groundMeters));

    // A still frame, waited for at most 30 s, so a frame that never finishes fails the test.
    PremultipliedImage image;
    const auto renderStill = [&] {
        bool done = false;
        std::exception_ptr error;
        gfx::BackendScope guard{*frontend.getBackend()};
        map.renderStill([&](const std::exception_ptr& e) {
            error = e;
            if (!e) {
                image = frontend.readStillImage();
            }
            done = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!done && std::chrono::steady_clock::now() < deadline) {
            loop.runOnce();
        }
        return done && !error;
    };
    ASSERT_TRUE(renderStill()) << "the still frame before the change did not finish";

    // A 100 m square 300 m north of the centre.
    const LatLng patch{center.latitude() + 0.3 / 111.32, center.longitude()};
    const double half = 0.05 / 111.32;
    const double halfLon = half / std::cos(patch.latitude() * M_PI / 180.0);
    map.getStyle().getSource("patch")->as<style::GeoJSONSource>()->setGeoJSON(
        Geometry<double>{Polygon<double>{{{patch.longitude() - halfLon, patch.latitude() - half},
                                          {patch.longitude() + halfLon, patch.latitude() - half},
                                          {patch.longitude() + halfLon, patch.latitude() + half},
                                          {patch.longitude() - halfLon, patch.latitude() + half},
                                          {patch.longitude() - halfLon, patch.latitude() - half}}}});
    ASSERT_TRUE(renderStill()) << "the still frame after the change did not finish";

    // Where the patch's centre is drawn: on the ground's height, through the camera's projection
    // matrix, as the terrain vertex shader places it. Top-left origin.
    const auto state = frontend.getTransformState();
    ASSERT_TRUE(state);
    const Point<double> world = Projection::project(patch, state->getScale());
    vec4 clip;
    matrix::transformMat4(clip, vec4{{world.x, world.y, groundMeters, 1.0}}, state->getProjectionMatrix());
    const double px = (clip[0] / clip[3] + 1.0) * image.size.width / 2.0;
    const double py = (1.0 - clip[1] / clip[3]) * image.size.height / 2.0;
    ASSERT_GT(px, 8.0);
    ASSERT_GT(py, 8.0);
    ASSERT_LT(px, image.size.width - 8.0);
    ASSERT_LT(py, image.size.height - 8.0);

    int red = 0;
    for (int y = static_cast<int>(py) - 3; y <= static_cast<int>(py) + 3; ++y) {
        for (int x = static_cast<int>(px) - 3; x <= static_cast<int>(px) + 3; ++x) {
            const uint8_t* p = image.data.get() + (static_cast<size_t>(y) * image.size.width + x) * 4;
            red += (p[0] > 200 && p[1] < 60 && p[2] < 60) ? 1 : 0;
        }
    }
    EXPECT_EQ(red, 49) << "the patch is not drawn at (" << px << ", " << py << ") in the first still frame";
}

namespace {

// Flat ground under one fill that covers the whole view, coloured from its feature's "color".
const char* swatchStyle = R"STYLE({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"],
      "encoding": "mapbox",
      "maxzoom": 11,
      "tileSize": 256
    },
    "swatch": {"type": "geojson", "data": {"type": "FeatureCollection", "features": []}}
  },
  "terrain": {"source": "dem", "exaggeration": 1.0},
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "white"}},
    {"id": "swatch", "type": "fill", "source": "swatch",
     "paint": {"fill-color": ["get", "color"], "fill-antialias": false}}
  ]
})STYLE";

} // namespace

// A still image read right after a frame holds that frame's drape. The drape targets and the
// frame are only committed to the GPU, not waited for, so reading the frame must wait for it; an
// image read before the GPU finished shows the previous frame's colour, or none. Each frame
// changes the drape's colour, so a stale read fails whichever frame it happens in.
TEST(TerrainDrape, AStillImageReadRightAfterTheFrameHoldsThatFramesDrape) {
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    const std::string dem = flatDemTile(groundMeters);
    fileSource->tileResponse = [&](const Resource&) {
        Response res;
        res.data = std::make_shared<std::string>(dem);
        return res;
    };
    HeadlessFrontend frontend{{512, 512}, 1};
    MapAdapter map(frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()));
    map.getStyle().loadJSON(swatchStyle);
    const LatLng center{46.0, 10.0};
    map.jumpTo(CameraOptions().withCenter(center).withZoom(14.0).withPitch(45.0).withCenterAltitude(groundMeters));

    const auto setColor = [&](const char* color) {
        mapbox::feature::property_map properties;
        properties["color"] = std::string(color);
        map.getStyle().getSource("swatch")->as<style::GeoJSONSource>()->setGeoJSON(mapbox::feature::feature<double>{
            Polygon<double>{{{9.5, 45.5}, {10.5, 45.5}, {10.5, 46.5}, {9.5, 46.5}, {9.5, 45.5}}},
            std::move(properties)});
    };

    // A still frame, read as soon as it is done (as HeadlessFrontend::render does), waited for at
    // most 30 s.
    PremultipliedImage image;
    const auto renderStill = [&] {
        bool done = false;
        std::exception_ptr error;
        gfx::BackendScope guard{*frontend.getBackend()};
        map.renderStill([&](const std::exception_ptr& e) {
            error = e;
            if (!e) {
                image = frontend.readStillImage();
            }
            done = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!done && std::chrono::steady_clock::now() < deadline) {
            loop.runOnce();
        }
        return done && !error;
    };

    // The lower half of the view is ground near the camera, all of it under the swatch.
    const auto countColor = [&](bool red) {
        int count = 0;
        for (uint32_t y = image.size.height / 2; y < image.size.height - 8; ++y) {
            for (uint32_t x = 8; x < image.size.width - 8; ++x) {
                const uint8_t* p = image.data.get() + (static_cast<size_t>(y) * image.size.width + x) * 4;
                const bool isRed = p[0] > 200 && p[1] < 60 && p[2] < 60;
                const bool isBlue = p[0] < 60 && p[1] < 60 && p[2] > 200;
                count += (red ? isRed : isBlue) ? 1 : 0;
            }
        }
        return count;
    };
    for (int frame = 0; frame < 8; ++frame) {
        const bool red = frame % 2 == 0;
        setColor(red ? "#f00" : "#00f");
        ASSERT_TRUE(renderStill()) << "still frame " << frame << " did not finish";
        const int area = static_cast<int>((image.size.height / 2 - 8) * (image.size.width - 16));
        ASSERT_GT(area, 0);
        EXPECT_GT(countColor(red), area * 99 / 100)
            << "frame " << frame << " (" << (red ? "red" : "blue") << ") read before its drape was drawn; "
            << countColor(!red) << " px still show the previous frame";
    }
}
