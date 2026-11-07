#pragma once

#include <mln/util/chrono.hpp>

#include <string>
#include <optional>

namespace mln {
namespace http {

class CacheControl {
public:
    static CacheControl parse(const std::string&);

    std::optional<uint64_t> maxAge;
    bool mustRevalidate = false;
    /// `no-cache` (not the field-qualified `no-cache="..."`, which only restricts the named fields):
    /// the response must be revalidated before every reuse.
    bool noCache = false;
    /// `no-store`: the response must not be reused.
    bool noStore = false;

    /// When the response expires: now for `no-cache` or `no-store` (it is stale at once), after
    /// `max-age` otherwise, or none.
    std::optional<Timestamp> toTimePoint() const;
};

std::optional<Timestamp> parseRetryHeaders(const std::optional<std::string>& retryAfter,
                                           const std::optional<std::string>& xRateLimitReset);

} // namespace http
} // namespace mln
