#pragma once

#include <mln/gfx/dynamic_texture.hpp>
#include <mln/text/glyph.hpp>
#include <mln/style/image_impl.hpp>
#include <mln/util/hash.hpp>

#include <unordered_map>

namespace mln {

namespace gfx {

using DynamicTexturePtr = std::shared_ptr<gfx::DynamicTexture>;

class GlyphAtlas {
public:
    GlyphPositions glyphPositions;
    std::vector<TextureHandle> textureHandles;
    DynamicTexturePtr dynamicTexture;
};

class ImageAtlas {
public:
    ImagePositions iconPositions;
    ImagePositions patternPositions;
    std::vector<TextureHandle> textureHandles;
    DynamicTexturePtr dynamicTexture;
};

class DynamicTextureAtlas {
public:
    DynamicTextureAtlas(Context& context_)
        : context(context_) {}
    ~DynamicTextureAtlas() = default;

    GlyphAtlas uploadGlyphs(const GlyphMap& glyphs);
    ImageAtlas uploadIconsAndPatterns(const ImageMap& icons,
                                      const ImageMap& patterns,
                                      const ImageVersionMap& versionMap);

    void removeTextures(const std::vector<TextureHandle>& textureHandles, const DynamicTexturePtr& dynamicTexture);
    void removeUnusedDynamicTextures();
    /// Returns released bins whose frames have completed on the GPU to their textures. Render
    /// thread, once per frame; never waits.
    void retireCompletedBins();
    /// Released bins still waiting for their frames, over the atlas's textures.
    size_t numPendingBins();

private:
    // Image allocations are shared only within one immutable image revision and usage (icon or pattern).
    // A replaced image is a new revision, so it gets a fresh bin with its own pixels even while
    // layouts still hold the previous revision's bin.
    struct ImageAllocationKey {
        const DynamicTexture* dynamicTexture;
        const style::Image::Impl* image;
        ImageType type;

        bool operator==(const ImageAllocationKey&) const = default;
    };
    struct ImageAllocationKeyHasher {
        size_t operator()(const ImageAllocationKey& key) const {
            return util::hash(key.dynamicTexture, key.image, key.type);
        }
    };
    struct ImageAllocation {
        // Keeps the revision alive so its address can't be reused by another image while the bin exists.
        Immutable<style::Image::Impl> image;
        int32_t binId;
    };
    struct BinKey {
        const DynamicTexture* dynamicTexture;
        int32_t binId;

        bool operator==(const BinKey&) const = default;
    };
    struct BinKeyHasher {
        size_t operator()(const BinKey& key) const { return util::hash(key.dynamicTexture, key.binId); }
    };

    std::optional<TextureHandle> reserveImage(const DynamicTexturePtr&,
                                              const Immutable<style::Image::Impl>&,
                                              ImageType);
    /// `unused`: a reservation of this pass, rolled back before anything used it
    void releaseTexture(const DynamicTexturePtr&, const TextureHandle&, bool unused = false);

    Context& context;
    std::vector<DynamicTexturePtr> dynamicTextures;
    std::unordered_map<TexturePixelType, DynamicTexturePtr> dummyDynamicTexture;
    std::unordered_map<ImageAllocationKey, ImageAllocation, ImageAllocationKeyHasher> imageAllocations;
    std::unordered_map<BinKey, ImageAllocationKey, BinKeyHasher> imageAllocationKeys;
    std::mutex mutex;
};

} // namespace gfx
} // namespace mln
