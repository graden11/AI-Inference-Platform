#include "../../include/middleware/RateLimitMiddleware.h"
#include <muduo/base/Logging.h>

namespace http {
namespace middleware {

void RateLimitMiddleware::before(HttpRequest& req)
{
    // not needed — rate limit is checked inline by caller or
    // the middleware fires an HttpResponse exception on rejection.
    // For simplicity, callers check allow() explicitly and
    // construct a 429 response when denied.
    (void)req;
}

bool RateLimitMiddleware::allow(const std::string& ip)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    auto& bucket = buckets_[ip];

    // Initialize bucket on first access
    if (bucket.lastFill == std::chrono::steady_clock::time_point{}) {
        bucket.tokens = static_cast<double>(burst_);
        bucket.lastFill = now;
    } else {
        double elapsed = std::chrono::duration<double>(now - bucket.lastFill).count();
        bucket.tokens = std::min(static_cast<double>(burst_),
                                 bucket.tokens + elapsed * rate_);
        bucket.lastFill = now;
    }

    bool allowed = false;
    if (bucket.tokens >= 1.0) {
        bucket.tokens -= 1.0;
        allowed = true;
    }

    maybeCleanup();
    return allowed;
}

void RateLimitMiddleware::maybeCleanup()
{
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - lastCleanup_).count() < 60.0)
        return;
    lastCleanup_ = now;

    for (auto it = buckets_.begin(); it != buckets_.end(); ) {
        if (std::chrono::duration<double>(now - it->second.lastFill).count() > 60.0)
            it = buckets_.erase(it);
        else
            ++it;
    }
}

} // namespace middleware
} // namespace http
