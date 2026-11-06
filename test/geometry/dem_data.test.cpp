#include <mln/test/util.hpp>

#include <mln/util/image.hpp>
#include <mln/util/tileset.hpp>
#include <mln/geometry/dem_data.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace mln;

auto fakeImage = [](Size s) {
    PremultipliedImage img = PremultipliedImage(s);

    for (size_t i = 0; i < img.bytes(); i++) {
        img.data[i] = (i + 1) % 4 == 0 ? 1 : std::rand() % 255;
    }
    return img;
};

TEST(DEMData, ConstructorMapbox) {
    PremultipliedImage image = fakeImage({16, 16});
    DEMData demdata(image, Tileset::RasterEncoding::Mapbox);

    EXPECT_EQ(demdata.dim, 16);
    EXPECT_EQ(demdata.stride, 20);
    EXPECT_EQ(demdata.getImage()->bytes(), size_t(20 * 20 * 4));
};

TEST(DEMData, ConstructorTerrarium) {
    PremultipliedImage image = fakeImage({16, 16});
    DEMData demdata(image, Tileset::RasterEncoding::Terrarium);

    EXPECT_EQ(demdata.dim, 16);
    EXPECT_EQ(demdata.stride, 20);
    EXPECT_EQ(demdata.getImage()->bytes(), size_t(20 * 20 * 4));
};

TEST(DEMData, InitialBackfill) {
    PremultipliedImage image1 = fakeImage({4, 4});
    DEMData dem1(image1, Tileset::RasterEncoding::Mapbox);

    // both border columns of DEM data are initially equal to the nearest column of data
    bool verticalBorderMatch = true;
    for (int y = 0; y < 4; y++) {
        for (int x : {-2, -1}) {
            if (dem1.get(x, y) != dem1.get(0, y)) {
                verticalBorderMatch = false;
            }
        }
        for (int x : {4, 5}) {
            if (dem1.get(x, y) != dem1.get(3, y)) {
                verticalBorderMatch = false;
            }
        }
    }
    EXPECT_TRUE(verticalBorderMatch);

    // both border rows of DEM data are initially equal to the nearest row of data
    bool horizontalBorderMatch = true;
    for (int x = 0; x < 4; x++) {
        for (int y : {-2, -1}) {
            if (dem1.get(x, y) != dem1.get(x, 0)) {
                horizontalBorderMatch = false;
            }
        }
        for (int y : {4, 5}) {
            if (dem1.get(x, y) != dem1.get(x, 3)) {
                horizontalBorderMatch = false;
            }
        }
    }
    EXPECT_TRUE(horizontalBorderMatch);

    // every corner cell is initially equal to the closest corner pixel
    for (int x : {-2, -1}) {
        for (int y : {-2, -1}) {
            EXPECT_EQ(dem1.get(x, y), dem1.get(0, 0));
        }
        for (int y : {4, 5}) {
            EXPECT_EQ(dem1.get(x, y), dem1.get(0, 3));
        }
    }
    for (int x : {4, 5}) {
        for (int y : {-2, -1}) {
            EXPECT_EQ(dem1.get(x, y), dem1.get(3, 0));
        }
        for (int y : {4, 5}) {
            EXPECT_EQ(dem1.get(x, y), dem1.get(3, 3));
        }
    }
};

TEST(DEMData, BackfillNeighbor) {
    PremultipliedImage image1 = fakeImage({4, 4});
    DEMData dem0(image1, Tileset::RasterEncoding::Mapbox);

    PremultipliedImage image2 = fakeImage({4, 4});
    DEMData dem1(image2, Tileset::RasterEncoding::Mapbox);

    // Each neighbour fills two pixels deep, so the border carries the neighbour's two
    // nearest columns/rows rather than just its edge.
    dem0.backfillBorder(dem1, -1, 0);
    for (int y = 0; y < 4; y++) {
        // dx = -1, dy = 0: the left border of dem0 is the right edge of dem1
        EXPECT_EQ(dem0.get(-1, y), dem1.get(3, y));
        EXPECT_EQ(dem0.get(-2, y), dem1.get(2, y));
    }

    dem0.backfillBorder(dem1, 0, -1);
    // backfills TopCenter neighbor
    for (int x = 0; x < 4; x++) {
        EXPECT_EQ(dem0.get(x, -1), dem1.get(x, 3));
        EXPECT_EQ(dem0.get(x, -2), dem1.get(x, 2));
    }

    dem0.backfillBorder(dem1, 1, 0);
    // backfills Right neighbor
    for (int y = 0; y < 4; y++) {
        EXPECT_EQ(dem0.get(4, y), dem1.get(0, y));
        EXPECT_EQ(dem0.get(5, y), dem1.get(1, y));
    }

    dem0.backfillBorder(dem1, 0, 1);
    // backfills BottomCenter neighbor
    for (int x = 0; x < 4; x++) {
        EXPECT_EQ(dem0.get(x, 4), dem1.get(x, 0));
        EXPECT_EQ(dem0.get(x, 5), dem1.get(x, 1));
    }

    dem0.backfillBorder(dem1, -1, 1);
    // backfills BottomLeft neighbor
    EXPECT_EQ(dem0.get(-1, 4), dem1.get(3, 0));
    EXPECT_EQ(dem0.get(-2, 5), dem1.get(2, 1));

    dem0.backfillBorder(dem1, 1, 1);
    // backfills BottomRight neighbor
    EXPECT_EQ(dem0.get(4, 4), dem1.get(0, 0));
    EXPECT_EQ(dem0.get(5, 5), dem1.get(1, 1));

    dem0.backfillBorder(dem1, -1, -1);
    // backfills TopLeft neighbor
    EXPECT_EQ(dem0.get(-1, -1), dem1.get(3, 3));
    EXPECT_EQ(dem0.get(-2, -2), dem1.get(2, 2));

    dem0.backfillBorder(dem1, 1, -1);
    // backfills TopRight neighbor
    EXPECT_EQ(dem0.get(4, -1), dem1.get(0, 3));
    EXPECT_EQ(dem0.get(5, -2), dem1.get(1, 2));
};

// backfillBorder reports whether it changed the border, and only a change gives the data a new
// revision: what was made from the texels (an uploaded texture, a stitched DEM) is redone then
// and only then.
TEST(DEMData, BackfillReportsAChangeAndRevisesTheData) {
    PremultipliedImage image0 = fakeImage({4, 4});
    PremultipliedImage image1 = fakeImage({4, 4});
    image1.data[0] = static_cast<uint8_t>(image0.data[3 * 4] + 1); // not dem0's own edge
    DEMData dem0(image0, Tileset::RasterEncoding::Mapbox);
    const DEMData dem1(image1, Tileset::RasterEncoding::Mapbox);
    EXPECT_NE(dem0.getRevision(), dem1.getRevision()) << "two decoded tiles share a revision";

    const uint64_t fresh = dem0.getRevision();
    EXPECT_TRUE(dem0.backfillBorder(dem1, 1, 0)) << "the right border held the tile's own edge";
    const uint64_t backfilled = dem0.getRevision();
    EXPECT_NE(backfilled, fresh);

    EXPECT_FALSE(dem0.backfillBorder(dem1, 1, 0)) << "the same neighbour again changed the border";
    EXPECT_EQ(dem0.getRevision(), backfilled);
}

TEST(DEMData, TerrariumNoData) {
    // A fresh image is all zeroes, i.e. terrarium no-data, bar one pixel put at a real
    // 256 m (129 * 256 - 32768). 128 would decode to 0 m and prove nothing.
    PremultipliedImage image({4, 4});
    for (size_t i = 3; i < image.bytes(); i += 4) {
        image.data[i] = 1; // alpha, as fakeImage does
    }
    image.data[(1 * 4 + 1) * 4] = 129;

    DEMData terrarium(image, Tileset::RasterEncoding::Terrarium);
    EXPECT_EQ(terrarium.get(0, 0), 0);
    EXPECT_EQ(terrarium.get(1, 1), 256);
    // No-data must not drag the tile's elevation range below sea level.
    EXPECT_EQ(terrarium.getMinElevation(), 0);
    EXPECT_EQ(terrarium.getMaxElevation(), 256);

    // Mapbox encoding has no no-data value; the same pixels decode as they always did.
    DEMData mapbox(image, Tileset::RasterEncoding::Mapbox);
    EXPECT_EQ(mapbox.get(0, 0), -10000);
};

namespace {
// A Terrarium image whose pixel (x, y) is `meters[y * dim + x]`, to the nearest 1/256 m.
PremultipliedImage terrariumImage(uint32_t dim, const std::vector<double>& meters) {
    PremultipliedImage image({dim, dim});
    for (size_t i = 0; i < meters.size(); ++i) {
        const auto value = static_cast<uint32_t>(std::lround((meters[i] + 32768.0) * 256.0));
        image.data[i * 4] = static_cast<uint8_t>(value >> 16);
        image.data[i * 4 + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        image.data[i * 4 + 2] = static_cast<uint8_t>(value & 0xFF);
        image.data[i * 4 + 3] = 255;
    }
    return image;
}

void expectRangeHoldsEveryHeight(const DEMData& dem) {
    for (int32_t y = 0; y < dem.dim; ++y) {
        for (int32_t x = 0; x < dem.dim; ++x) {
            EXPECT_LE(dem.getMinElevation(), dem.get(x, y)) << "at (" << x << ", " << y << ")";
            EXPECT_GE(dem.getMaxElevation(), dem.get(x, y)) << "at (" << x << ", " << y << ")";
        }
    }
}
} // namespace

// The tile's elevation range bounds the terrain for frustum culling and for the tap raycast, so it
// must hold every height the tile decodes to, fraction of a metre included: whole metres
// outwards, never toward zero. A peak 255/256 m above 1000 m tops out at 1001; ground 5.5 m below
// sea level bottoms out at -6.
TEST(DEMData, ElevationRangeHoldsEveryDecodedHeight) {
    const DEMData dem(terrariumImage(2, {1000.0 + 255.0 / 256.0, 1000.0, -5.5, 12.25}),
                      Tileset::RasterEncoding::Terrarium);
    EXPECT_FLOAT_EQ(dem.get(0, 0), 1000.0f + 255.0f / 256.0f);
    EXPECT_FLOAT_EQ(dem.get(0, 1), -5.5f);
    EXPECT_EQ(dem.getMaxElevation(), 1001);
    EXPECT_EQ(dem.getMinElevation(), -6);
    expectRangeHoldsEveryHeight(dem);
}

// Whole-metre heights give the range they always did.
TEST(DEMData, ElevationRangeOfWholeMetres) {
    const DEMData dem(terrariumImage(2, {1000.0, 20.0, -5.0, 3.0}), Tileset::RasterEncoding::Terrarium);
    EXPECT_EQ(dem.getMaxElevation(), 1000);
    EXPECT_EQ(dem.getMinElevation(), -5);
    expectRangeHoldsEveryHeight(dem);
}

// Ground entirely below sea level, all of it fraction: the range is [-1, 0], not [0, 0].
TEST(DEMData, ElevationRangeBelowSeaLevel) {
    const DEMData dem(terrariumImage(2, {-0.5, -0.25, -0.75, -0.5}), Tileset::RasterEncoding::Terrarium);
    EXPECT_EQ(dem.getMaxElevation(), 0);
    EXPECT_EQ(dem.getMinElevation(), -1);
    expectRangeHoldsEveryHeight(dem);
}
