#include <mln/storage/cache_generation.hpp>
#include <mln/storage/database_file_source.hpp>
#include <mln/storage/file_source_manager.hpp>
#include <mln/storage/resource.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/test/util.hpp>
#include <mln/util/run_loop.hpp>
#include <mln/util/timer.hpp>

#include <gtest/gtest.h>

#include <functional>

using namespace mln;

TEST(DatabaseFileSource, PauseResume) {
    util::RunLoop loop;

    std::shared_ptr<FileSource> dbfs = FileSourceManager::get()->getFileSource(FileSourceType::Database,
                                                                               ResourceOptions{});
    dbfs->pause();

    const Resource res{Resource::Unknown, "http://127.0.0.1:3000/test", {}, Resource::LoadingMethod::CacheOnly};
    auto req = dbfs->request(res, [&](const Response&) { loop.stop(); });

    util::Timer resumeTimer;
    resumeTimer.start(Milliseconds(5), Duration::zero(), [dbfs] { dbfs->resume(); });

    loop.run();
}

TEST(DatabaseFileSource, VolatileResource) {
    util::RunLoop loop;

    std::shared_ptr<FileSource> dbfs = FileSourceManager::get()->getFileSource(FileSourceType::Database,
                                                                               ResourceOptions{});

    Resource resource{Resource::Unknown, "http://127.0.0.1:3000/test", {}, Resource::LoadingMethod::CacheOnly};
    Response response{};
    response.data = std::make_shared<std::string>("Cached value");
    std::unique_ptr<mln::AsyncRequest> req;

    dbfs->forward(resource, response, [&] {
        req = dbfs->request(resource, [&](Response res1) {
            EXPECT_EQ(nullptr, res1.error);
            ASSERT_TRUE(res1.data.get());
            EXPECT_FALSE(res1.noContent);
            EXPECT_EQ("Cached value", *res1.data);
            resource.storagePolicy = Resource::StoragePolicy::Volatile;
            req = dbfs->request(resource, [&](Response res2) {
                req.reset();
                ASSERT_TRUE(res2.error.get());
                EXPECT_TRUE(res2.noContent);
                EXPECT_EQ(Response::Error::Reason::NotFound, res2.error->reason);
                EXPECT_EQ("Not found in offline database", res2.error->message);
                loop.stop();
            });
        });
    });
    loop.run();
}

TEST(DatabaseFileSource, VolatileResourceIsNotStored) {
    util::RunLoop loop;

    std::shared_ptr<FileSource> dbfs = FileSourceManager::get()->getFileSource(FileSourceType::Database,
                                                                               ResourceOptions{});

    Resource resource{
        Resource::Unknown, "http://127.0.0.1:3000/volatile-not-stored", {}, Resource::LoadingMethod::CacheOnly};
    resource.storagePolicy = Resource::StoragePolicy::Volatile;
    Response response{};
    response.data = std::make_shared<std::string>("Volatile value");

    // Forwarding a volatile response must not write it to the database,
    // so a regular request for the same resource finds nothing.
    dbfs->forward(resource, response, nullptr);
    resource.storagePolicy = Resource::StoragePolicy::Permanent;
    std::unique_ptr<mln::AsyncRequest> req = dbfs->request(resource, [&](Response res) {
        req.reset();
        loop.stop();
        EXPECT_TRUE(res.noContent);
        ASSERT_TRUE(res.error.get());
        EXPECT_EQ(Response::Error::Reason::NotFound, res.error->reason);
    });
    loop.run();
}

// Invalidating, clearing or resetting the cache moves ambientCacheGeneration once done, so what the
// map learnt from the cache (the raster-dem tiles known to be empty) lapses.
TEST(DatabaseFileSource, ChangingTheCacheMovesItsGeneration) {
    util::RunLoop loop;
    auto dbfs = std::static_pointer_cast<DatabaseFileSource>(
        FileSourceManager::get()->getFileSource(FileSourceType::Database, ResourceOptions{}));
    const auto expectMoved = [&](const std::string& what,
                                 const std::function<void(std::function<void(std::exception_ptr)>)>& change) {
        const auto before = ambientCacheGeneration().load();
        change([&](std::exception_ptr error) {
            EXPECT_FALSE(error) << what;
            EXPECT_GT(ambientCacheGeneration().load(), before) << what;
            loop.stop();
        });
        loop.run();
    };
    expectMoved("invalidateAmbientCache", [&](auto done) { dbfs->invalidateAmbientCache(std::move(done)); });
    expectMoved("clearAmbientCache", [&](auto done) { dbfs->clearAmbientCache(std::move(done)); });
    expectMoved("resetDatabase", [&](auto done) { dbfs->resetDatabase(std::move(done)); });
}
