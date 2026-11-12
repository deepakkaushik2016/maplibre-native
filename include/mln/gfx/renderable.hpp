#pragma once

#include <mln/util/size.hpp>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>

namespace mln {
namespace gfx {

class RenderableResource {
protected:
    explicit RenderableResource() = default;

public:
    virtual ~RenderableResource() = default;
    RenderableResource(const RenderableResource&) = delete;
    RenderableResource& operator=(const RenderableResource&) = delete;

    virtual void bind() = 0;
};

class Renderable {
public:
    enum class SwapBehaviour {
        NoFlush,
        Flush
    };

protected:
    Renderable(const Size size_, std::unique_ptr<RenderableResource> resource_)
        : size(size_),
          resource(std::move(resource_)) {}
    virtual ~Renderable() = default;

public:
    /// Returns the renderable dimensions in physical pixels.
    Size getSize() const { return size; }

    /// Identifies this renderable, with its current resource, for the life of the process. Unlike an
    /// address it is never reused, so a cache that outlives the renderable (a drawable remembering
    /// the last render pass it drew in, while the drape target it drew into is freed) can tell a new
    /// renderable from a freed one without holding or dereferencing it. A new resource is a new id.
    uint64_t getUniqueID() const noexcept { return uniqueID; }

    template <typename T>
    T& getResource() const {
        assert(resource);
        return static_cast<T&>(*resource);
    }

    bool hasResource() const { return resource != nullptr; }

    void setResource(std::unique_ptr<RenderableResource> resource_) {
        resource = std::move(resource_);
        uniqueID = nextUniqueID();
    }

    virtual void wait() {}

    bool operator!=(const Renderable& other) const { return resource.get() != other.resource.get(); }

private:
    static uint64_t nextUniqueID() noexcept {
        static std::atomic<uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }

    Size size;
    uint64_t uniqueID = nextUniqueID();

protected:
    void setRenderableSize(const Size& size_) noexcept { size = size_; }

    std::unique_ptr<RenderableResource> resource;
};

} // namespace gfx
} // namespace mln
