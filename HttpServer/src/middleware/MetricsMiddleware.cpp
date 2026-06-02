#include "../../include/middleware/MetricsMiddleware.h"
#include "../../include/utils/MetricsCollector.h"
#include "../../include/utils/LogUtil.h"

#include <muduo/base/Timestamp.h>

#include <nlohmann/json.hpp>

namespace http {
namespace middleware {

namespace {
thread_local std::string t_path;
thread_local std::string t_method;
thread_local muduo::Timestamp t_start;
}

void MetricsMiddleware::before(HttpRequest &request)
{
    t_path = request.path();
    t_method = request.methodString();
    t_start = request.receiveTime();
    (void)request;
}

void MetricsMiddleware::after(HttpResponse &response)
{
    if (!t_start.valid())
        return;

    int64_t latency_us = muduo::Timestamp::now().microSecondsSinceEpoch() -
                         t_start.microSecondsSinceEpoch();
    bool is_error = (static_cast<int>(response.getStatusCode()) >= 400);

    MetricsCollector::instance().record(t_path, t_method, latency_us, is_error);

    // Structured JSON access log
    nlohmann::json entry;
    entry["request_id"] = response.getRequestId();
    entry["method"] = t_method;
    entry["path"] = t_path;
    entry["status"] = static_cast<int>(response.getStatusCode());
    entry["latency_ms"] = latency_us / 1000.0;
    entry["client_ip"] = response.getClientIp();

    LOG_ACCESS(entry.dump());

    t_start = muduo::Timestamp();  // invalidate
}

} // namespace middleware
} // namespace http
