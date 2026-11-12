#pragma once

#include <mln/gfx/debug_group.hpp>
#include <mln/util/color.hpp>
#include <mln/gfx/renderable.hpp>

#include <cstdint>
#include <optional>

namespace mln {
namespace gfx {

class RenderPassDescriptor {
public:
    Renderable& renderable;
    std::optional<Color> clearColor;
    std::optional<float> clearDepth;
    std::optional<int32_t> clearStencil;

    /// What may be kept of a render pass after it ends: the renderable's identity rather than a
    /// reference to it, which dangles once the renderable is freed (a drape target's texture is
    /// freed while the drawables that drew into it live on).
    struct Key {
        uint64_t renderableID = 0;
        std::optional<Color> clearColor;
        std::optional<float> clearDepth;
        std::optional<int32_t> clearStencil;
        bool operator==(const Key&) const = default;
    };

    Key key() const {
        return {.renderableID = renderable.getUniqueID(),
                .clearColor = clearColor,
                .clearDepth = clearDepth,
                .clearStencil = clearStencil};
    }
};

class RenderPass {
protected:
    RenderPass() = default;

    friend class DebugGroup<RenderPass>;
    virtual void pushDebugGroup(const char* name) = 0;
    virtual void popDebugGroup() = 0;
    virtual void addDebugSignpost(const char*) {}

public:
    virtual ~RenderPass() = default;
    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;

    DebugGroup<RenderPass> createDebugGroup(const char* name) { return {*this, name}; }
};

} // namespace gfx
} // namespace mln
