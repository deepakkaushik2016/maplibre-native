#include <mln/test/util.hpp>

#include <mln/gfx/renderable.hpp>

#include <cstddef>
#include <memory>
#include <new>

namespace {

class TestRenderable final : public mln::gfx::Renderable {
public:
    explicit TestRenderable(mln::Size size_)
        : Renderable(size_, nullptr) {}

    void resize(const mln::Size& size_) { setRenderableSize(size_); }
};

class TestResource final : public mln::gfx::RenderableResource {
public:
    void bind() override {}
};

} // namespace

TEST(Renderable, MaintainsCanonicalPhysicalSize) {
    TestRenderable renderable({256, 128});
    EXPECT_EQ(renderable.getSize(), (mln::Size{256, 128}));

    renderable.resize({512, 384});
    EXPECT_EQ(renderable.getSize(), (mln::Size{512, 384}));
}

// Caches that outlive a renderable (a drawable's last render pass, a context's clip-mask states)
// key on its unique id. A renderable made where a freed one lived must not pass for it: this one
// is built in the very bytes of the first, so the address is certainly reused.
TEST(Renderable, AUniqueIDIsNeverReusedEvenWhenTheAddressIs) {
    alignas(TestRenderable) std::byte storage[sizeof(TestRenderable)];
    auto* first = new (storage) TestRenderable({64, 64});
    const auto firstID = first->getUniqueID();
    first->~TestRenderable();

    auto* second = new (storage) TestRenderable({64, 64});
    EXPECT_EQ(static_cast<void*>(second), static_cast<void*>(first));
    EXPECT_NE(second->getUniqueID(), firstID);
    second->~TestRenderable();
}

// A renderable whose resource is replaced or dropped renders somewhere new: it gets a new id.
TEST(Renderable, ANewResourceIsANewUniqueID) {
    TestRenderable renderable({64, 64});
    const auto atFirst = renderable.getUniqueID();
    renderable.setResource(std::make_unique<TestResource>());
    const auto withResource = renderable.getUniqueID();
    EXPECT_NE(withResource, atFirst);
    renderable.setResource(nullptr);
    EXPECT_NE(renderable.getUniqueID(), withResource);
    EXPECT_NE(renderable.getUniqueID(), atFirst);
}

// A ring of texture slots parks one resource while it draws into another. The resource handed back
// is the one that was in place, and the renderable now draws somewhere new: it gets a new id, also
// when an earlier resource comes back, so nothing cached against the earlier id is taken for it.
TEST(Renderable, ExchangingAResourceReturnsTheOldOneAndIsANewUniqueID) {
    TestRenderable renderable({64, 64});
    auto first = std::make_unique<TestResource>();
    auto* const firstAddress = first.get();
    renderable.setResource(std::move(first));
    const auto withFirst = renderable.getUniqueID();

    auto second = std::make_unique<TestResource>();
    auto* const secondAddress = second.get();
    auto parked = renderable.exchangeResource(std::move(second));
    EXPECT_EQ(parked.get(), firstAddress);
    EXPECT_EQ(&renderable.getResource<TestResource>(), secondAddress);
    const auto withSecond = renderable.getUniqueID();
    EXPECT_NE(withSecond, withFirst);

    // The first resource comes back: the same resource, but a new id all the same.
    auto parkedSecond = renderable.exchangeResource(std::move(parked));
    EXPECT_EQ(parkedSecond.get(), secondAddress);
    EXPECT_EQ(&renderable.getResource<TestResource>(), firstAddress);
    EXPECT_NE(renderable.getUniqueID(), withFirst);
    EXPECT_NE(renderable.getUniqueID(), withSecond);

    // Exchanging for nothing takes the resource out and still renews the id.
    const auto beforeTakingOut = renderable.getUniqueID();
    auto takenOut = renderable.exchangeResource(nullptr);
    EXPECT_EQ(takenOut.get(), firstAddress);
    EXPECT_FALSE(renderable.hasResource());
    EXPECT_NE(renderable.getUniqueID(), beforeTakingOut);
}
