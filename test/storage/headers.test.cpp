#include <mln/test/util.hpp>
#include <mln/util/chrono.hpp>
#include <mln/util/http_header.hpp>

TEST(HTTPHeader, Parsing) {
    using namespace mln;

    http::CacheControl cc;

    cc = http::CacheControl::parse(R"#()#");
    ASSERT_FALSE(bool(cc.maxAge));
    EXPECT_FALSE(cc.mustRevalidate);

    cc = http::CacheControl::parse(R"#(max-age =34)#");
    ASSERT_TRUE(bool(cc.maxAge));
    EXPECT_EQ(34u, *cc.maxAge);
    EXPECT_FALSE(cc.mustRevalidate);

    cc = http::CacheControl::parse(R"#(,max-age=1)#");
    ASSERT_TRUE(bool(cc.maxAge));
    EXPECT_EQ(1u, *cc.maxAge);
    EXPECT_FALSE(cc.mustRevalidate);

    cc = http::CacheControl::parse(R"#(max-age=-1)#");
    ASSERT_FALSE(bool(cc.maxAge));
    EXPECT_FALSE(cc.mustRevalidate);

    cc = http::CacheControl::parse(R"#(max-age=foo)#");
    ASSERT_FALSE(bool(cc.maxAge));
    EXPECT_FALSE(cc.mustRevalidate);

    cc = http::CacheControl::parse(R"#(max-age="34,max-age="22,max-age=28)#");
    ASSERT_TRUE(bool(cc.maxAge));
    EXPECT_EQ(28u, *cc.maxAge);
    EXPECT_FALSE(cc.mustRevalidate);

    cc = http::CacheControl::parse(R"#(max-age=3,max-age="34)#");
    ASSERT_TRUE(bool(cc.maxAge));
    EXPECT_EQ(3u, *cc.maxAge);
    EXPECT_FALSE(cc.mustRevalidate);

    cc = http::CacheControl::parse(R"#(max-age="\",max-age=4,")#");
    ASSERT_FALSE(bool(cc.maxAge));
    EXPECT_FALSE(cc.mustRevalidate);

    cc = http::CacheControl::parse(R"#(private, max-age=0, no-cache)#");
    ASSERT_TRUE(bool(cc.maxAge));
    EXPECT_EQ(0u, *cc.maxAge);
    EXPECT_FALSE(cc.mustRevalidate);

    cc = http::CacheControl::parse(R"#(max-age=0, no-cache, no-store)#");
    ASSERT_TRUE(bool(cc.maxAge));
    EXPECT_EQ(0u, *cc.maxAge);
    EXPECT_FALSE(cc.mustRevalidate);

    cc = http::CacheControl::parse(R"#(,   private   , max-bar=3 , no-cache,  "\,",,foo=",",,max-age=32)#");
    ASSERT_TRUE(bool(cc.maxAge));
    EXPECT_EQ(32u, *cc.maxAge);
    EXPECT_FALSE(cc.mustRevalidate);

    cc = http::CacheControl::parse(R"#(max-age=3600, must-revalidate)#");
    ASSERT_TRUE(bool(cc.maxAge));
    EXPECT_EQ(3600u, *cc.maxAge);
    EXPECT_TRUE(cc.mustRevalidate);

    cc = http::CacheControl::parse(R"#(no-cache="Expires,Via",max-age=3600, must-revalidate)#");
    ASSERT_TRUE(bool(cc.maxAge));
    EXPECT_EQ(3600u, *cc.maxAge);
    EXPECT_TRUE(cc.mustRevalidate);
    // Field-qualified: only the named fields may not be reused.
    EXPECT_FALSE(cc.noCache);
}

// no-store and no-cache make the response stale at once, whatever max-age says.
TEST(HTTPHeader, NoStoreAndNoCache) {
    using namespace mln;

    auto cc = http::CacheControl::parse("no-store");
    EXPECT_TRUE(cc.noStore);
    EXPECT_FALSE(cc.noCache);
    ASSERT_TRUE(cc.toTimePoint());
    EXPECT_LE(*cc.toTimePoint(), util::now());

    cc = http::CacheControl::parse("public, no-cache");
    EXPECT_TRUE(cc.noCache);
    EXPECT_FALSE(cc.noStore);
    ASSERT_TRUE(cc.toTimePoint());
    EXPECT_LE(*cc.toTimePoint(), util::now());

    cc = http::CacheControl::parse("max-age=3600, no-cache");
    EXPECT_TRUE(cc.noCache);
    EXPECT_LE(*cc.toTimePoint(), util::now());

    cc = http::CacheControl::parse("max-age=3600");
    EXPECT_FALSE(cc.noCache);
    EXPECT_FALSE(cc.noStore);
    EXPECT_GT(*cc.toTimePoint(), util::now());

    // Neither is a prefix match.
    cc = http::CacheControl::parse("no-storex, no-cachey");
    EXPECT_FALSE(cc.noStore);
    EXPECT_FALSE(cc.noCache);
}
