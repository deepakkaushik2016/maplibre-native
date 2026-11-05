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

    /// The height in metres at pixel (x, y), as the shaders decode it: not rounded.
    float get(int32_t x, int32_t y) const;
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
