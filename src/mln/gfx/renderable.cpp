#include <mln/gfx/renderable.hpp>

#include <atomic>

namespace mln {
namespace gfx {

uint64_t Renderable::nextUniqueID() noexcept {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

} // namespace gfx
} // namespace mln
