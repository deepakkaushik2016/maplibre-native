#pragma once

#include <mln/util/size.hpp>

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
    /// renderable from a freed one without holding or dereferencing it. The resource is private and
    /// only setResource changes it, so every new (or dropped) resource is a new id.
    uint64_t getUniqueID() const noexcept { return uniqueID; }

    template <typename T>
    T& getResource() const {
        assert(resource);
        return static_cast<T&>(*resource);
    }

    bool hasResource() const { return resource != nullptr; }

    /// Replaces (or, with nullptr, drops) the resource, and gives the renderable a new unique id.
    void setResource(std::unique_ptr<RenderableResource> resource_) {
        resource = std::move(resource_);
        uniqueID = nextUniqueID();
    }

    virtual void wait() {}

protected:
    void setRenderableSize(const Size& size_) noexcept { size = size_; }

private:
    /// Out of line, so the process has one counter even where symbols are hidden across shared
    /// libraries.
    static uint64_t nextUniqueID() noexcept;

    Size size;
    std::unique_ptr<RenderableResource> resource;
    uint64_t uniqueID = nextUniqueID();
};

} // namespace gfx
} // namespace mln
