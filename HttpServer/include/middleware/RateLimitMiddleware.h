#pragma once

#include "Middleware.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace http {
namespace middleware {

// Token-bucket rate limiter with per-IP state.
// Default: 100 requests/second per IP.
class RateLimitMiddleware : public Middleware {
public:
    explicit RateLimitMiddleware(int requestsPerSec = 100,
                                int burstSize = 200)
        : rate_(requestsPerSec), burst_(burstSize) {}

    void before(HttpRequest& req) override;
    void after(HttpResponse&) override {}

    // Returns true if the request should be allowed.
    bool allow(const std::string& ip);

private:
    struct Bucket {
        double tokens;
        std::chrono::steady_clock::time_point lastFill;
    };

    double rate_;
    int burst_;
    std::mutex mutex_;
    std::unordered_map<std::string, Bucket> buckets_;

    // Auto-clean entries older than 60 seconds without activity
    void maybeCleanup();
    std::chrono::steady_clock::time_point lastCleanup_;
};

} // namespace middleware
} // namespace http
