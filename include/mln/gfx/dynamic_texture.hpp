#pragma once

#include <mln/gfx/frame_serials.hpp>
#include <mln/gfx/types.hpp>
#include <mln/util/image.hpp>
#include <mln/util/rect.hpp>

#include <mapbox/shelf-pack.hpp>

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace mln {
namespace gfx {

class Context;
class Texture2D;
class UploadPass;
using Texture2DPtr = std::shared_ptr<Texture2D>;

class TextureHandle {
public:
    TextureHandle(const mapbox::Bin& bin)
        : id(bin.id),
          rectangle(bin.x, bin.y, bin.w, bin.h),
          needsUpload(bin.refcount() == 1) {};
    ~TextureHandle() = default;

    int32_t getId() const { return id; }
    const Rect<uint16_t>& getRectangle() const { return rectangle; }
    bool isUploadNeeded() const { return needsUpload; }

    bool operator==(const TextureHandle& other) const { return (id == other.id); }

    struct Hasher {
        size_t operator()(const TextureHandle& texHandle) const { return texHandle.id; }
    };

private:
    int32_t id = 0;
    Rect<uint16_t> rectangle;
    bool needsUpload = false;

    friend class DynamicTexture;
};

/// A texture that packs images (glyphs, icons, patterns) into bins, shared by tile layouts.
///
/// A bin whose last handle is released is not free at once: a frame still on the GPU may have
/// drawn it, and the next image packed into it would be written while that frame samples it (on
/// Metal, by a worker thread, with no wait). The release is stamped with the backend's
/// `FrameSerials::releaseStamp()`, and the bin goes back to the shelf pack once the GPU has
/// completed that serial: checked at each allocation, and at the start of each frame
/// (`retireCompletedBins`). Until then it counts as used. Nothing here waits for the GPU; an
/// allocation that finds no room fails as before, and the atlas moves on to another texture.
///
/// Without frame serials (WebGPU, whose queue orders a write after the draws before it, and tests)
/// a released bin is free at once, as before.
class DynamicTexture {
public:
    DynamicTexture(Context& context, Size size, TexturePixelType pixelType);
    virtual ~DynamicTexture();

    const Texture2DPtr& getTexture() const;
    TexturePixelType getPixelFormat() const;
    /// No live handle. Bins waiting for the GPU don't count: dropping the texture is safe, since a
    /// frame in flight keeps its own reference to the GPU texture.
    bool isEmpty() const;

    std::optional<TextureHandle> reserveSize(const Size& size, int32_t uniqueId);

    template <typename Image>
    std::optional<TextureHandle> addImage(const Image& image, int32_t uniqueId = -1) {
        return addImage(image.data ? image.data.get() : nullptr, image.size, uniqueId);
    }
    std::optional<TextureHandle> addImage(const uint8_t* pixelData, const Size& imageSize, int32_t uniqueId = -1);

    virtual void uploadImage(const uint8_t* pixelData, TextureHandle& texHandle);
    virtual void uploadDeferredImages(UploadPass&) {};
    /// Releases a handle. Returns whether it was the bin's last one. That bin is reused only once
    /// every frame that may have drawn it has completed on the GPU.
    bool removeTexture(const TextureHandle& texHandle);
    /// Releases a handle reserved in this allocation pass and never uploaded or handed to a layout
    /// (an atlas rolling back a set that didn't fit). A bin it reserved fresh (one that needed an
    /// upload) no frame can have drawn, so it is free at once; a shared one is released as by
    /// `removeTexture`. Returns whether it was the bin's last handle.
    bool releaseUnusedReservation(const TextureHandle& texHandle);
    /// Returns to the shelf pack the released bins whose frames have completed.
    void retireCompletedBins();
    /// Released bins still waiting for their frames.
    size_t numPendingBins() const;

protected:
    DynamicTexture(Size size, TexturePixelType pixelType, std::shared_ptr<FrameSerials> frameSerials = {});

    /// A bin goes back to the shelf pack (with `mutex` held): drop anything still queued for it.
    virtual void onBinReleased(const TextureHandle&) {}

    mapbox::ShelfPack shelfPack;
    Texture2DPtr texture;
    TexturePixelType pixelFormat;
    int numTextures = 0;
    mutable std::mutex mutex;

private:
    bool releaseHandleLocked(const TextureHandle&, bool deferred);
    void releaseBin(mapbox::Bin& bin);
    void retireCompletedBinsLocked();

    struct PendingBin {
        int32_t id;
        uint64_t stamp;
    };
    std::shared_ptr<FrameSerials> frameSerials;
    /// Released bins in release order, and so in stamp order. An entry is stale once its id was
    /// reserved again, or released again with a later stamp: `pendingStamps` holds the live one.
    std::deque<PendingBin> pendingBins;
    std::unordered_map<int32_t, uint64_t> pendingStamps;
};

} // namespace gfx
} // namespace mln
