#include <mln/test/util.hpp>
#include <mln/test/stub_file_source.hpp>
#include <mln/test/map_adapter.hpp>

#include <mln/gfx/context.hpp>
#include <mln/gfx/headless_frontend.hpp>
#include <mln/gfx/rendering_stats.hpp>
#include <mln/map/camera.hpp>
#include <mln/map/map_options.hpp>
#include <mln/style/image.hpp>
#include <mln/style/style.hpp>
#include <mln/util/image.hpp>
#include <mln/util/io.hpp>
#include <mln/util/run_loop.hpp>

#include <cstdlib>
#include <memory>
#include <string>

using namespace mln;

namespace {

// The Alps under Innsbruck at 3x, from the render tests' terrain tiles, with a grid of red icons
// over them. The icons ignore collision, so only the terrain depth can hide one: those behind a
// ridge are tested against the packed terrain depth and not drawn.
std::string iconsOverTheAlps() {
    std::string points;
    for (int i = 0; i < 50; ++i) {
        for (int j = 0; j < 60; ++j) {
            points += std::string(points.empty() ? "" : ",") + "[" + std::to_string(11.45 + 0.004 * i) + "," +
                      std::to_string(47.21 + 0.004 * j) + "]";
        }
    }
    return R"STYLE({
  "version": 8,
  "sources": {
    "dem": {
      "type": "raster-dem",
      "tiles": ["http://example.com/dem/{z}-{x}-{y}.terrain.png"],
      "maxzoom": 12,
      "tileSize": 256
    },
    "points": {"type": "geojson", "data": {"type": "MultiPoint", "coordinates": [)STYLE" +
           points + R"STYLE(]}}
  },
  "terrain": {"source": "dem", "exaggeration": 3},
  "layers": [
    {"id": "background", "type": "background", "paint": {"background-color": "white"}},
    {"id": "icons", "type": "symbol", "source": "points",
     "layout": {"icon-image": "dot", "icon-allow-overlap": true, "icon-ignore-placement": true}}
  ]
})STYLE";
}

std::unique_ptr<style::Image> redDot() {
    PremultipliedImage image({6, 6});
    for (uint32_t i = 0; i < 6 * 6; ++i) {
        uint8_t* px = image.data.get() + i * 4;
        px[0] = 255;
        px[1] = 0;
        px[2] = 0;
        px[3] = 255;
    }
    return std::make_unique<style::Image>("dot", std::move(image), 1.0f);
}

// The view of the occlusion-debug render test. With no centre altitude the camera stands at sea
// level, inside the 3x relief, and the terrain mesh it covers is not round the DEM tile under the
// centre (12/2179/1434), which the icons there stand on.
const CameraOptions firstView = CameraOptions().withCenter(LatLng{47.34487, 11.52517}).withZoom(13.0).withPitch(60.0);

// Views from 6 km up over that DEM tile, above most of the 3x relief, looking south and
// north across its ridges.
CameraOptions secondView(double bearing) {
    return CameraOptions()
        .withCenter(LatLng{47.3655, 11.5576})
        .withZoom(13.0)
        .withPitch(65.0)
        .withBearing(bearing)
        .withCenterAltitude(6000.0);
}

struct AlpsScene {
    util::RunLoop loop;
    std::shared_ptr<StubFileSource> fileSource = std::make_shared<StubFileSource>(ResourceOptions::Default(),
                                                                                  ClientOptions());
    HeadlessFrontend frontend{{512, 512}, 1};
    MapAdapter map{frontend,
                   MapObserver::nullObserver(),
                   fileSource,
                   MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize())};

    AlpsScene() {
        fileSource->tileResponse = [](const Resource& resource) {
            Response res;
            const std::string name = resource.url.substr(resource.url.rfind('/') + 1);
            try {
                res.data = std::make_shared<std::string>(
                    util::read_file("metrics/integration/tiles/terrain-shading/" + name));
            } catch (const std::exception&) {
                res.noContent = true;
            }
            return res;
        };
        map.getStyle().loadJSON(iconsOverTheAlps());
        map.getStyle().addImage(redDot());
    }

    PremultipliedImage render(const CameraOptions& camera) {
        map.jumpTo(camera);
        return frontend.render(map).image;
    }
};

int redPixels(const PremultipliedImage& image) {
    int count = 0;
    for (size_t i = 0; i < image.bytes(); i += 4) {
        count += (image.data[i] > 200 && image.data[i + 1] < 60 && image.data[i + 2] < 60) ? 1 : 0;
    }
    return count;
}

int differingPixels(const PremultipliedImage& image, const PremultipliedImage& reference, int tolerance = 8) {
    if (image.size != reference.size) {
        return -1;
    }
    int count = 0;
    const size_t n = static_cast<size_t>(image.size.width) * image.size.height;
    for (size_t i = 0; i < n; ++i) {
        for (size_t c = 0; c < 4; ++c) {
            if (std::abs(int{image.data[i * 4 + c]} - int{reference.data[i * 4 + c]}) > tolerance) {
                ++count;
                break;
            }
        }
    }
    return count;
}

} // namespace

// The terrain drops a DEM tile's texture in a frame whose mesh covers nothing round that tile, and
// a layer standing on the tile (here the icons) makes it again on demand, in getTerrainData. When
// the next view's mesh covers the tile, the mesh draws on that texture. On OpenGL each DEM tile is
// also packed into one texture array for the instanced terrain depth pass, and a texture made
// again on demand was not: the mesh tiles on it were left out of the depth, and icons behind their
// ridges showed through until the tile's DEM changed. The frame must be the one a map that opened
// on the second view draws.
TEST(TerrainDepth, ADEMTextureMadeAgainForAnotherLayerIsInTheDepth) {
    for (const double bearing : {180.0, 0.0}) {
        AlpsScene reference;
        const PremultipliedImage expected = reference.render(secondView(bearing));
        AlpsScene scene;
        scene.render(firstView);
        // The first view must take the path under test, or the comparison below proves nothing.
        ASSERT_GT(scene.frontend.getBackend()->getContext().renderingStats().numDEMTexturesRemade, 0)
            << "no DEM texture was made again on demand from the first view";
        const PremultipliedImage image = scene.render(secondView(bearing));

        ASSERT_GT(redPixels(expected), 0) << "no icon is drawn looking at bearing " << bearing;
        EXPECT_EQ(differingPixels(image, expected), 0)
            << "looking at bearing " << bearing << ", the frame differs from the one a map that opened "
            << "on the view draws (icons hidden by the terrain depth): " << redPixels(image) << " red px against "
            << redPixels(expected);
    }
}
