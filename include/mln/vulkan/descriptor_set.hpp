#pragma once

#include <mln/gfx/uniform_buffer.hpp>
#include <mln/vulkan/buffer_resource.hpp>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <queue>
#include <span>

namespace mln {
namespace vulkan {

class CommandEncoder;

/// Rate limit for a log line about a condition that may repeat every frame (the dummy texture
/// bound in place of an image not created yet).
class FallbackLogLimiter {
public:
    explicit FallbackLogLimiter(std::chrono::steady_clock::duration interval_)
        : interval(interval_) {}

    /// Counts one occurrence at `now`. When a line is due (the first one, then once `interval`
    /// has passed since the last), returns the occurrences it covers, this one included; else 0.
    uint64_t count(std::chrono::steady_clock::time_point now);

private:
    const std::chrono::steady_clock::duration interval;
    std::mutex mutex;
    std::chrono::steady_clock::time_point lastLine{};
    bool logged = false;
    uint64_t unlogged = 0;
};

enum class DescriptorSetType : uint8_t {
    Global,
    Layer,
    DrawableUniform,
    DrawableImage,
    Count,
};

struct DescriptorPoolGrowable {
    struct PoolInfo {
        vk::UniqueDescriptorPool pool;
        uint32_t remainingSets{0};
        std::queue<std::vector<vk::DescriptorSet>> unusedSets;

        PoolInfo(vk::UniqueDescriptorPool&& pool_, uint32_t remainingSets_)
            : pool(std::move(pool_)),
              remainingSets(remainingSets_) {}
    };

    const uint32_t maxSets{0};
    const uint32_t descriptorStoragePerSet{0};
    const uint32_t descriptorUniformsPerSet{0};
    const uint32_t descriptorTexturesPerSet{0};
    const float growFactor{1.5f};

    std::vector<PoolInfo> pools;
    int32_t currentPoolIndex{-1};

    PoolInfo& current() { return pools[currentPoolIndex]; }

    DescriptorPoolGrowable() = default;
    DescriptorPoolGrowable(uint32_t maxSets_,
                           uint32_t descriptorStoragePerSet_,
                           uint32_t descriptorUniformsPerSet_,
                           uint32_t descriptorTexturesPerSet_,
                           float growFactor_ = 1.5f)
        : maxSets(maxSets_),
          descriptorStoragePerSet(descriptorStoragePerSet_),
          descriptorUniformsPerSet(descriptorUniformsPerSet_),
          descriptorTexturesPerSet(descriptorTexturesPerSet_),
          growFactor(growFactor_) {}
};

class DescriptorSet {
public:
    DescriptorSet(Context& context_, DescriptorSetType type_);
    virtual ~DescriptorSet();

    void allocate();

    virtual void markDirty();
    void bind(CommandEncoder& encoder);

protected:
    void createDescriptorPool(DescriptorPoolGrowable& growablePool);

    /// Called by `update` before it writes the current frame's set. A set bound into the frame's
    /// command buffer must not be written again while that buffer records or runs (the buffer is
    /// invalidated: VUID-vkCmdBindDescriptorSets-commandBuffer-recording). If it was, the whole
    /// group is retired until the frame completes and a fresh one is allocated, every slot dirty.
    void prepareUpdate(uint8_t frameIndex);

    /// Hands the sets back to their pool once the frames that may use them have completed.
    void retire();

protected:
    Context& context;
    DescriptorSetType type;

    std::vector<bool> dirty;
    std::vector<vk::DescriptorSet> descriptorSets;
    /// Per frame slot, the frame serial (Context::getFrameSerial) its set was last bound in.
    std::vector<uint64_t> boundFrame;
    int32_t descriptorPoolIndex{-1};
};

class UniformDescriptorSet : public DescriptorSet {
public:
    UniformDescriptorSet(Context& context_, DescriptorSetType type_);
    ~UniformDescriptorSet() override = default;

    void update(const gfx::UniformBufferArray& uniforms,
                uint32_t descriptorStartIndex,
                uint32_t descriptorStorageCount,
                uint32_t descriptorUniformCount);
};

class ImageDescriptorSet : public DescriptorSet {
public:
    ImageDescriptorSet(Context& context_);
    ~ImageDescriptorSet() override = default;

    void markDirty() override;
    const std::chrono::duration<double>& getLastModified() const { return lastModified; }

    void update(const std::array<gfx::Texture2DPtr, shaders::maxTextureCountPerShader>& textures);

protected:
    std::chrono::duration<double> lastModified;
};

} // namespace vulkan
} // namespace mln
