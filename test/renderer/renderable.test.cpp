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
