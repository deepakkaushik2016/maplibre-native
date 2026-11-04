#include <mln/geometry/dem_data.hpp>
#include <mln/math/clamp.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace mln {

DEMData::DEMData(const PremultipliedImage& _image, Tileset::RasterEncoding _encoding)
    : dim(_image.size.height),
      // extra two pixels per row for border backfilling on either edge. Two, not one, so a
      // bilinear fetch centred on an edge pixel still has a full neighbourhood - and so the
      // hillshade prepare pass can derive a 1px ring outside the tile, which needs DEM
      // samples two pixels out. Matches maplibre-gl-js #8302.
      stride(dim + 4),
      encoding(_encoding) {
    image = std::make_shared<PremultipliedImage>(Size(static_cast<uint32_t>(stride), static_cast<uint32_t>(stride)));
    if (_image.size.height != _image.size.width) {
        throw std::runtime_error("raster-dem tiles must be square.");
    }

    auto* dest = reinterpret_cast<uint32_t*>(image->data.get()) + stride * 2 + 2;
    auto* source = reinterpret_cast<uint32_t*>(_image.data.get());
    for (int32_t y = 0; y < dim; y++) {
        memcpy(dest, source, dim * 4);
        dest += stride;
        source += dim;
    }

    // in order to avoid flashing seams between tiles, here we are initially
    // populating a 2px border of pixels around the image with the data of the
    // nearest pixel from the image. this data is eventually replaced when the
    // tile's neighboring tiles are loaded and the accurate data can be
    // backfilled using DEMData#backfillBorder

    auto* data = reinterpret_cast<uint32_t*>(image->data.get());
    if (dim > 0) {
        for (int32_t y = -2; y < dim + 2; y++) {
            auto* src = data + idx(0, std::clamp(y, 0, dim - 1));
            auto* dst = data + idx(0, y);
            if (y < 0 || y >= dim) {
                memcpy(dst, src, dim * 4);
            }
            dst[-2] = dst[-1] = src[0];
            dst[dim] = dst[dim + 1] = src[dim - 1];
        }
    }

    // The elevation range of the tile, used to give the tile a height when testing
    // it against the view frustum (see util::tileCover): terrain rising towards the
    // camera is visible from further away than its flat footprint suggests. Computed
    // once here, on the worker thread that decodes the tile, rather than per frame.
    // The border is excluded: it is a copy of the edge pixels until neighbouring
    // tiles backfill it, so it holds no elevation this tile does not already have.
    // In whole metres rounded outwards, so the range holds every height the tile decodes to,
    // fraction of a metre included: a range rounded toward zero can cull a tile whose peak
    // pokes into view, or end a tap's raycast short of the ground.
    if (dim > 0) {
        float lowest = get(0, 0);
        float highest = lowest;
        for (int32_t y = 0; y < dim; y++) {
            for (int32_t x = 0; x < dim; x++) {
                const float value = get(x, y);
                lowest = std::min(lowest, value);
                highest = std::max(highest, value);
            }
        }
        minElevation = static_cast<int32_t>(std::floor(lowest));
        maxElevation = static_cast<int32_t>(std::ceil(highest));
    }
}

DEMData::DEMData(std::shared_ptr<PremultipliedImage> borderedImage, Tileset::RasterEncoding _encoding)
    : dim(borderedImage ? static_cast<int32_t>(borderedImage->size.height) - 4 : 0),
      stride(dim + 4),
      encoding(_encoding),
      image(std::move(borderedImage)) {
    if (!image || image->size.width != image->size.height || dim <= 0) {
        throw std::runtime_error("raster-dem data must be square, with a 2-texel border.");
    }
}

// This function takes the DEMData from a neighboring tile and backfills the
// edge/corner data in order to create a two pixel "buffer" of image data around
// the tile. This is necessary because the hillshade formula calculates the
// dx/dz, dy/dz derivatives at each pixel of the tile by querying the 8
// surrounding pixels, and if we don't have the pixel buffer we get seams at
// tile boundaries.
void DEMData::backfillBorder(const DEMData& borderTileData, int8_t dx, int8_t dy) {
    auto& o = borderTileData;

    // Tiles from the same source should always be of the same dimensions.
    assert(dim == o.dim);

    // We determine the pixel range to backfill based which corner/edge
    // `borderTileData` represents. For example, dx = -1, dy = -1 represents the
    // upper left corner of the base tile, so we only need to backfill one pixel
    // at coordinates (-1, -1) of the tile image.
    int32_t xMin = dx * dim;
    int32_t xMax = dx * dim + dim;
    int32_t yMin = dy * dim;
    int32_t yMax = dy * dim + dim;

    if (dx == -1)
        xMin = xMax - 2;
    else if (dx == 1)
        xMax = xMin + 2;

    if (dy == -1)
        yMin = yMax - 2;
    else if (dy == 1)
        yMax = yMin + 2;

    int32_t ox = -dx * dim;
    int32_t oy = -dy * dim;

    auto* dest = reinterpret_cast<uint32_t*>(image->data.get());
    auto* source = reinterpret_cast<uint32_t*>(o.image->data.get());

    for (int32_t y = yMin; y < yMax; y++) {
        for (int32_t x = xMin; x < xMax; x++) {
            dest[idx(x, y)] = source[idx(x + ox, y + oy)];
        }
    }
}

float DEMData::get(const int32_t x, const int32_t y) const {
    const uint8_t* value = image->data.get() + idx(x, y) * 4;
    // The R, G, B digits count the encoding's steps up from its base: Mapbox Terrain-RGB
    // 0.1 m steps from -10000 m, Terrarium 1/256 m steps from -32768 m. Counted as an integer
    // and scaled in double, so a height the encoding holds exactly (a whole metre, say) decodes
    // exactly, fraction of a metre kept, as maplibre-gl-js DEMData decodes it. The shaders'
    // single-precision unpack vector (6553.6f is not exact) reads within a millimetre of this.
    const auto steps = (uint32_t{value[0]} << 16) | (uint32_t{value[1]} << 8) | uint32_t{value[2]};
    // Reading no-data as -32768 m would drag the tile's elevation range (and the frustum
    // test built on it) down with it.
    if (encoding == Tileset::RasterEncoding::Terrarium) {
        return steps == 0 ? 0.0f : static_cast<float>(steps / 256.0 - 32768.0);
    }
    return static_cast<float>(steps * 0.1 - 10000.0);
}

const std::array<float, 4>& DEMData::getUnpackVector() const {
    // https://www.mapbox.com/help/access-elevation-data/#mapbox-terrain-rgb
    static const std::array<float, 4> unpackMapbox = {{6553.6f, 25.6f, 0.1f, 10000.0f}};
    // https://aws.amazon.com/public-datasets/terrain/
    static const std::array<float, 4> unpackTerrarium = {{256.0f, 1.0f, 1.0f / 256.0f, 32768.0f}};

    return encoding == Tileset::RasterEncoding::Terrarium ? unpackTerrarium : unpackMapbox;
}

} // namespace mln
