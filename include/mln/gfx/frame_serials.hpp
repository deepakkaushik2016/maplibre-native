#pragma once

#include <atomic>
#include <cstdint>

namespace mln {
namespace gfx {

/// How far the GPU has got through the work a backend submitted, for a resource that the CPU frees
/// on any thread while frames in flight may still read it (a glyph or icon atlas bin).
///
/// A serial counts the backend's units of GPU work in submission order: frames on Vulkan and
/// OpenGL, render-pass command buffers on Metal. The render thread advances both serials; any
/// thread may read them. Neither ever waits for the GPU: `completed` is what the backend has
/// already observed (a frame slot's fence it waited for anyway, or the status of a tracked command
/// buffer).
class FrameSerials {
public:
    /// The serial of the newest unit of GPU work handed out (being encoded, or submitted).
    uint64_t submitted() const noexcept { return submittedSerial.load(std::memory_order_acquire); }

    /// Every unit of GPU work up to this serial has completed.
    uint64_t completed() const noexcept { return completedSerial.load(std::memory_order_acquire); }

    /// The serial a resource released now is stamped with: it may be read by the work handed out
    /// so far, and by the next unit, which may already draw from state built before the release
    /// (a drawable not yet rebuilt). It may be reused once `completed()` reaches it.
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
