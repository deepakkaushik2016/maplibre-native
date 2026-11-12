#pragma once

#include <mln/math/clamp.hpp>
#include <mln/util/image.hpp>
#include <mln/util/tileset.hpp>

#include <memory>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

namespace mln {

class DEMData {
public:
    DEMData(const PremultipliedImage& image, Tileset::RasterEncoding encoding);
    /// Adopts an image already laid out as this class keeps it: `dim` + 4 texels square, the
    /// data inset by its 2-texel border. The elevation range is not computed (it stays 0).
    DEMData(std::shared_ptr<PremultipliedImage> borderedImage, Tileset::RasterEncoding encoding);
    /// Copies the neighbouring tile's pixels at offset (`dx`, `dy`) into the border, and returns
    /// whether that changed it: false when the border held them already (the same neighbour
    /// again, or a reload of it with the same heights), so nothing made from it needs redoing.
    bool backfillBorder(const DEMData& borderTileData, int8_t dx, int8_t dy);

    /// Names the texels as they are now: unique to this data, and new whenever backfillBorder
    /// changes them, so a copy of them (an uploaded texture) can tell that it is stale.
    uint64_t getRevision() const { return revision; }

    /// The height in metres at pixel (x, y), as the shaders decode it: not rounded. Inline: the
    /// CPU samples the terrain under line labels' vertices and collision circles through it.
    float get(const int32_t x, const int32_t y) const {
        const uint8_t* value = image->data.get() + idx(x, y) * 4;
        // The R, G, B digits count the encoding's steps up from its base: Mapbox Terrain-RGB
        // 0.1 m steps from -10000 m, Terrarium 1/256 m steps from -32768 m. Counted as an integer
        // and scaled in double, so a height the encoding holds exactly (a whole metre, say) decodes
        // exactly, fraction of a metre kept, as maplibre-gl-js DEMData decodes it. The shaders'
        // single-precision unpack vector (6553.6f is not exact) reads within 2 mm of this.
        const auto steps = (uint32_t{value[0]} << 16) | (uint32_t{value[1]} << 8) | uint32_t{value[2]};
        // Reading no-data as -32768 m would drag the tile's elevation range (and the frustum
        // test built on it) down with it.
        if (encoding == Tileset::RasterEncoding::Terrarium) {
            return steps == 0 ? 0.0f : static_cast<float>(steps / 256.0 - 32768.0);
        }
        return static_cast<float>(steps * 0.1 - 10000.0);
    }
    const std::array<float, 4>& getUnpackVector() const;

    const PremultipliedImage* getImage() const { return &*image; }
    const std::shared_ptr<PremultipliedImage>& getImagePtr() const { return image; }

    /// Lowest elevation in the tile, in whole meters rounded down, excluding the backfilled border.
    int32_t getMinElevation() const { return minElevation; }
    /// Highest elevation in the tile, in whole meters rounded up, excluding the backfilled border.
    int32_t getMaxElevation() const { return maxElevation; }

    const int32_t dim;
    const int32_t stride;
    const Tileset::RasterEncoding encoding;

private:
    std::shared_ptr<PremultipliedImage> image;
    int32_t minElevation = 0;
    int32_t maxElevation = 0;
    uint64_t revision = newRevision();

    static uint64_t newRevision();

    size_t idx(const int32_t x, const int32_t y) const {
        assert(x >= -2);
        assert(x < dim + 2);
        assert(y >= -2);
        assert(y < dim + 2);
        return (y + 2) * stride + (x + 2);
    }
};

} // namespace mln
