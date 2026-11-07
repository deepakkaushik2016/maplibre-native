#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/image.hpp>
#include <mln/style/style.hpp>
#include <mln/util/image.hpp>
#include <mln/util/run_loop.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

using namespace mln;

namespace {

// A red line running north-south a little west of the map center, with a gradient and dashes (the
// line-gradient SDF shader), from a GeoJSON source that reaches the view's zoom. Flat ground at sea
// level under it when `terrain`, seen straight down at zoom 16.
std::string lineStyle(bool terrain) {
    const std::string terrainEntry = terrain ? R"("terrain": {"source": "dem", "exaggeration": 1.0},)" : "";
    return R"({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"],
      "encoding": "mapbox",
      "maxzoom": 11,
      "tileSize": 256
    },
    "line": {
      "type": "geojson",
      "maxzoom": 16,
      "lineMetrics": true,
      "data": {
        "type": "Feature",
        "properties": {},
        "geometry": {"type": "LineString", "coordinates": [[9.9995, 45.99], [9.9995, 46.01]]}
      }
    }
  },)" + terrainEntry +
           R"(
  "layers": [{"id": "line", "type": "line", "source": "line",
              "paint": {"line-width": 16, "line-dasharray": [2, 1],
                        "line-gradient": ["interpolate", ["linear"], ["line-progress"], 0, "red", 1, "red"]}}]
})";
}

std::string seaLevelDEMTile() {
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

struct RedPixels {
    int band = 0;    // in the band where the line is
    int outside = 0; // anywhere else
};

RedPixels renderLine(bool terrain) {
    util::RunLoop loop;
    auto fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(), ClientOptions());
    const std::string dem = seaLevelDEMTile();
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
    map.getStyle().loadJSON(lineStyle(terrain));
    map.jumpTo(CameraOptions().withCenter(LatLng{46.0, 10.0}).withZoom(16.0).withPitch(0.0));

    PremultipliedImage image;
    for (int i = 0; i < 4; ++i) {
        loop.runOnce();
        image = frontend.render(map).image;
    }

    // At zoom 16 the line, 0.0005° west of the center, lies about 47 px left of it (x 128).
    RedPixels red;
    for (uint32_t y = 16; y < 240; ++y) {
        for (uint32_t x = 0; x < 256; ++x) {
            const uint8_t* px = image.data.get() + (y * image.size.width + x) * 4;
            if (px[0] > 150 && px[1] < 90 && px[2] < 90) {
                (x >= 70 && x < 92 ? red.band : red.outside) += 1;
            }
        }
    }
    return red;
}

} // namespace

// Each draped vertex shader places its tile into the drape target it is drawing with
// apply_drape_transform. The line-gradient SDF shader (a line with both line-gradient and
// line-dasharray) did not, on any backend: under terrain the whole tile was drawn into every target
// it touched as if it filled it, so the dashes landed away from the line.
TEST(TerrainDrape, ADashedGradientLineLandsWhereItIs) {
    const RedPixels flat = renderLine(false);
    ASSERT_GT(flat.band, 1000) << "the line draws without terrain";
    ASSERT_LT(flat.outside, 50);

    const RedPixels draped = renderLine(true);
    EXPECT_GT(draped.band, flat.band * 8 / 10) << "outside " << draped.outside;
    EXPECT_LT(draped.outside, flat.band / 20) << "band " << draped.band;
}

namespace {

// A line draped on terrain is drawn into a drape target with an orthographic matrix, and the
// terrain mesh projects that target into the view. The line's antialiasing gamma must then be 1.0
// (perspective scaling happens when the mesh is projected), not the screen-space perspective ratio
// that the vertex shader computes from the drape matrix; that ratio is meaningless there and blurs
// or erodes the line's edges. With exaggeration 0 over a sea-level DEM the drape is a flat copy of
// the map, so every line variant must look the same with terrain on as with terrain off, up to the
// resampling of the drape texture - which the plain line variant (that already had the draped
// gamma) measures for us.
std::string makeStyle(const std::string& paint, bool terrain, bool lineMetrics) {
    std::string style = R"STYLE({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}-{x}-{y}.png"],
      "encoding": "mapbox",
      "maxzoom": 12,
      "tileSize": 256
    },
    "line": {
      "type": "geojson",)STYLE";
    style += lineMetrics ? R"("lineMetrics": true,)" : "";
    style += R"STYLE(
      "data": {
        "type": "Feature",
        "properties": {},
        "geometry": {
          "type": "LineString",
          "coordinates": [[9.986, 45.996], [9.993, 46.004], [10.0, 45.997], [10.007, 46.005], [10.014, 45.998]]
        }
      }
    }
  },)STYLE";
    if (terrain) {
        style += R"("terrain": {"source": "dem", "exaggeration": 0},)";
    }
    style += R"STYLE(
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "white"}},
    {"id": "line", "type": "line", "source": "line",
     "layout": {"line-cap": "butt", "line-join": "round"},
     "paint": )STYLE";
    style += paint;
    style += "}]}";
    return style;
}

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

// A solid red line-pattern image, so the pattern line looks like the solid one.
std::unique_ptr<style::Image> redPattern() {
    PremultipliedImage image({8, 8});
    for (uint32_t i = 0; i < 8 * 8; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        px[0] = 221;
        px[1] = 0;
        px[2] = 0;
        px[3] = 255;
    }
    return std::make_unique<style::Image>("red", std::move(image), 1.0f);
}

PremultipliedImage render(const std::string& paint, bool terrain, bool lineMetrics) {
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
    map.getStyle().loadJSON(makeStyle(paint, terrain, lineMetrics));
    map.getStyle().addImage(redPattern());
    map.jumpTo(CameraOptions().withCenter(LatLng{46.0, 10.0}).withZoom(14.0).withPitch(0.0));

    PremultipliedImage image;
    for (int i = 0; i < 4; ++i) {
        loop.runOnce();
        image = frontend.render(map).image;
    }
    return image;
}

struct Diff {
    double mean = 0;   // mean absolute channel difference over the whole image, 0-255
    int changed = 0;   // pixels with any channel off by more than 48
    int inked = 0;     // non-white pixels in the terrain-off render (how much line there is)
};

Diff compare(const PremultipliedImage& off, const PremultipliedImage& on) {
    Diff d;
    EXPECT_EQ(off.size, on.size);
    if (off.size != on.size) return d;
    const size_t pixels = static_cast<size_t>(off.size.width) * off.size.height;
    long long sum = 0;
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t* a = off.data.get() + i * 4;
        const uint8_t* b = on.data.get() + i * 4;
        int worst = 0;
        for (int c = 0; c < 4; ++c) {
            const int delta = std::abs(int(a[c]) - int(b[c]));
            sum += delta;
            worst = std::max(worst, delta);
        }
        d.changed += worst > 48 ? 1 : 0;
        d.inked += (a[0] < 250 || a[1] < 250 || a[2] < 250) ? 1 : 0;
    }
    d.mean = double(sum) / double(pixels * 4);
    return d;
}

Diff drapeDiff(const std::string& paint, bool lineMetrics = false) {
    return compare(render(paint, false, lineMetrics), render(paint, true, lineMetrics));
}

const char* const gradient =
    R"("line-gradient": ["interpolate", ["linear"], ["line-progress"], 0, "blue", 0.5, "lime", 1, "red"])";

// The plain line variant is the yardstick: it already uses the draped gamma, so what remains between
// its terrain-on and terrain-off renders is the drape texture's resampling. A variant without the
// draped gamma gets its edges smoothed by the wrong factor and lands well outside that.
void expectLikeTheSolidLine(const Diff& solid, const Diff& variant, const char* name) {
    SCOPED_TRACE(name);
    ASSERT_GT(variant.inked, 300) << "the line did not render";
    // Same edge treatment as the solid line, so at most the same share of pixels may move by more
    // than 48/255 (scaled by how much line there is - a dashed line has less ink), with 50% and
    // 10 px of slack for dash ends and joins resampled through the drape texture. On macOS Metal the
    // solid line moves 0 of ~2340 inked pixels; without the draped gamma the other variants moved
    // 690-2000 (about half their ink), so the bound sits two orders of magnitude below the defect.
    const double solidShare = double(solid.changed) / solid.inked;
    EXPECT_LE(variant.changed, solidShare * variant.inked * 1.5 + 10)
        << "solid changed " << solid.changed << "/" << solid.inked << ", " << name << " changed "
        << variant.changed << "/" << variant.inked << " (mean " << variant.mean << " vs " << solid.mean << ")";
}

} // namespace

TEST(TerrainLineDrape, EveryLineVariantDrapesLikeTheSolidLine) {
    const Diff solid = drapeDiff(R"({"line-color": "#d00", "line-width": 4})");
    ASSERT_GT(solid.inked, 300) << "the solid line did not render";
    const Diff dashed = drapeDiff(R"({"line-color": "#d00", "line-width": 4, "line-dasharray": [3, 2]})");
    const Diff grad = drapeDiff(std::string(R"({"line-width": 4, )") + gradient + "}", true);
    const Diff gradDashed = drapeDiff(
        std::string(R"({"line-width": 4, "line-dasharray": [3, 2], )") + gradient + "}", true);
    const Diff pattern = drapeDiff(R"({"line-pattern": "red", "line-width": 4})");

    std::printf("solid       changed %d / %d mean %.3f\n", solid.changed, solid.inked, solid.mean);
    std::printf("dashed      changed %d / %d mean %.3f\n", dashed.changed, dashed.inked, dashed.mean);
    std::printf("gradient    changed %d / %d mean %.3f\n", grad.changed, grad.inked, grad.mean);
    std::printf("grad+dashed changed %d / %d mean %.3f\n", gradDashed.changed, gradDashed.inked, gradDashed.mean);
    std::printf("pattern     changed %d / %d mean %.3f\n", pattern.changed, pattern.inked, pattern.mean);

    expectLikeTheSolidLine(solid, dashed, "line-dasharray (SDF)");
    expectLikeTheSolidLine(solid, grad, "line-gradient");
    expectLikeTheSolidLine(solid, gradDashed, "line-gradient + line-dasharray (gradient SDF)");
    expectLikeTheSolidLine(solid, pattern, "line-pattern");
}
