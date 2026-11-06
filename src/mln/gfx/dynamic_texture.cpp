#include <mln/gfx/dynamic_texture.hpp>
#include <mln/gfx/texture2d.hpp>
#include <mln/gfx/context.hpp>

namespace mln {
namespace gfx {

DynamicTexture::DynamicTexture(Size size, TexturePixelType pixelType, std::shared_ptr<FrameSerials> frameSerials_)
    : pixelFormat(pixelType),
      frameSerials(std::move(frameSerials_)) {
    mapbox::ShelfPack::ShelfPackOptions options;
    options.autoResize = false;
    shelfPack = mapbox::ShelfPack(size.width, size.height, options);
}

DynamicTexture::DynamicTexture(Context& context, Size size, TexturePixelType pixelType)
    : DynamicTexture(size, pixelType, context.getFrameSerials()) {
    texture = context.createTexture2D();
    texture->setSize(size);
    texture->setFormat(pixelType, gfx::TextureChannelDataType::UnsignedByte);
    texture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                      .wrapU = gfx::TextureWrapType::Clamp,
                                      .wrapV = gfx::TextureWrapType::Clamp});
}

DynamicTexture::~DynamicTexture() {
    // The bins go with the texture; a frame in flight holds its own reference to the GPU texture.
    if (frameSerials && !pendingStamps.empty()) {
        frameSerials->addPendingReleases(-static_cast<int64_t>(pendingStamps.size()));
    }
}

const gfx::Texture2DPtr& DynamicTexture::getTexture() const {
    assert(texture);
    return texture;
}

TexturePixelType DynamicTexture::getPixelFormat() const {
    return pixelFormat;
}

bool DynamicTexture::isEmpty() const {
    return (numTextures == 0);
}

std::optional<TextureHandle> DynamicTexture::reserveSize(const Size& size, int32_t uniqueId) {
    std::scoped_lock lock(mutex);
    retireCompletedBinsLocked();
    mapbox::Bin* bin = shelfPack.packOne(uniqueId, size.width, size.height);
    if (!bin) {
        return std::nullopt;
    }
    if (bin->refcount() == 1) {
        numTextures++;
        return TextureHandle(*bin);
    }
    if (const auto pending = pendingStamps.find(bin->id); pending != pendingStamps.end()) {
        // Released, and reserved again (the same glyph) before its frames completed: the bin still
        // holds that image, so it needs no upload. Taken while the pending reference still counts,
        // so the handle doesn't ask for one; then the pending reference goes.
        TextureHandle texHandle(*bin);
        pendingStamps.erase(pending);
        frameSerials->addPendingReleases(-1);
        shelfPack.unref(*bin);
        numTextures++;
        return texHandle;
    }
    return TextureHandle(*bin);
}

std::optional<TextureHandle> DynamicTexture::addImage(const uint8_t* pixelData,
                                                      const Size& imageSize,
                                                      int32_t uniqueId) {
    auto texHandle = reserveSize(imageSize, uniqueId);
    if (texHandle && texHandle->isUploadNeeded()) {
        uploadImage(pixelData, *texHandle);
    }
    return texHandle;
}

void DynamicTexture::uploadImage(const uint8_t* /*pixelData*/, gfx::TextureHandle& texHandle) {
    texHandle.needsUpload = false;
}

void DynamicTexture::releaseBin(mapbox::Bin& bin) {
    onBinReleased(TextureHandle(bin));
    shelfPack.unref(bin);
}

bool DynamicTexture::removeTexture(const TextureHandle& texHandle) {
    std::scoped_lock lock(mutex);
    return releaseHandleLocked(texHandle, /*deferred=*/frameSerials != nullptr);
}

bool DynamicTexture::releaseUnusedReservation(const TextureHandle& texHandle) {
    std::scoped_lock lock(mutex);
    // A handle that needed no upload shares a bin that was there before this pass (another
    // layout's, or one waiting for its frames), which a frame may have drawn.
    return releaseHandleLocked(texHandle, /*deferred=*/frameSerials && !texHandle.isUploadNeeded());
}

bool DynamicTexture::releaseHandleLocked(const TextureHandle& texHandle, bool deferred) {
    auto* bin = shelfPack.getBin(texHandle.getId());
    if (!bin || (!pendingStamps.empty() && pendingStamps.contains(bin->id))) {
        return false; // not ours, or already released
    }
    if (bin->refcount() > 1) {
        shelfPack.unref(*bin);
        return false;
    }
    numTextures--;
    if (!deferred) {
        releaseBin(*bin);
        return true;
    }
    // Keep the last reference until the frames that may have drawn the bin have completed. The
    // stamp is read under the lock, so the queue stays in stamp order.
    const auto stamp = frameSerials->releaseStamp();
    pendingBins.push_back({.id = bin->id, .stamp = stamp});
    pendingStamps[bin->id] = stamp;
    frameSerials->addPendingReleases(1);
    return true;
}

void DynamicTexture::retireCompletedBins() {
    std::scoped_lock lock(mutex);
    retireCompletedBinsLocked();
}

void DynamicTexture::retireCompletedBinsLocked() {
    if (pendingBins.empty()) {
        return;
    }
    const auto completed = frameSerials->completed();
    while (!pendingBins.empty() && pendingBins.front().stamp <= completed) {
        const auto [id, stamp] = pendingBins.front();
        pendingBins.pop_front();
        const auto live = pendingStamps.find(id);
        if (live == pendingStamps.end() || live->second != stamp) {
            continue; // reserved again since, or released again later
        }
        pendingStamps.erase(live);
        frameSerials->addPendingReleases(-1);
        if (auto* bin = shelfPack.getBin(id)) {
            releaseBin(*bin);
        }
    }
}

size_t DynamicTexture::numPendingBins() const {
    std::scoped_lock lock(mutex);
    return pendingStamps.size();
}

} // namespace gfx
} // namespace mln
