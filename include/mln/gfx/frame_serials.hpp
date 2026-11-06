#pragma once

#include <atomic>
#include <cstdint>

namespace mln {
namespace gfx {

/// How far the GPU has got through the work a backend submitted, for a resource that the CPU frees
/// on any thread while frames in flight may still read it (a glyph or icon atlas bin).
///
/// A serial counts the backend's frames, begun at Context::beginFrame, on every backend: a frame
/// encodes many render passes, and a drawable may sample a resource in any of them, so a stamp in
/// passes would not bound the readers. The render thread advances both serials; any thread may
/// read them. Neither ever waits for the GPU: `completed` is what the backend has already observed
/// (Vulkan: a frame slot's fence it waited for anyway; Metal: a frame has ended and every command
/// buffer it tracked has finished; OpenGL: a frame has been issued, see gl::Context).
class FrameSerials {
public:
    /// The serial of the newest frame begun (being encoded, or submitted).
    uint64_t submitted() const noexcept { return submittedSerial.load(std::memory_order_acquire); }

    /// Every frame up to this serial has completed on the GPU.
    uint64_t completed() const noexcept { return completedSerial.load(std::memory_order_acquire); }

    /// The serial a resource released now is stamped with: it may be read by the frames begun so
    /// far, and by the next frame, which may still draw from state built before the release (a
    /// drawable not yet rebuilt), in any of its passes. It may be reused once `completed()` reaches
    /// it: once the frame after the release has ended and its GPU work has completed.
    uint64_t releaseStamp() const noexcept { return submitted() + 1; }

    /// Render thread. Serials only grow.
    void setSubmitted(uint64_t serial) noexcept { raise(submittedSerial, serial); }
    void setCompleted(uint64_t serial) noexcept { raise(completedSerial, serial); }

    /// Resources released and not yet reusable, over every user of these serials. For tests and
    /// diagnostics: once the GPU is idle and frames keep coming, it drains to 0.
    int64_t pendingReleases() const noexcept { return pending.load(std::memory_order_relaxed); }
    void addPendingReleases(int64_t delta) noexcept { pending.fetch_add(delta, std::memory_order_relaxed); }

private:
    static void raise(std::atomic<uint64_t>& value, uint64_t serial) noexcept {
        auto current = value.load(std::memory_order_relaxed);
        while (current < serial && !value.compare_exchange_weak(current, serial, std::memory_order_acq_rel)) {
        }
    }

    std::atomic<uint64_t> submittedSerial{0};
    std::atomic<uint64_t> completedSerial{0};
    std::atomic<int64_t> pending{0};
};

} // namespace gfx
} // namespace mln
