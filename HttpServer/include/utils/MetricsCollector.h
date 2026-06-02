#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include <muduo/base/Timestamp.h>
#include <nlohmann/json.hpp>

struct EndpointMetrics {
    std::atomic<int64_t> total{0};
    std::atomic<int64_t> errors{0};
    std::atomic<int64_t> latency_us_sum{0};
    std::atomic<int64_t> latency_us_min{INT64_MAX};
    std::atomic<int64_t> latency_us_max{0};

    std::atomic<int64_t> bucket_10ms{0};
    std::atomic<int64_t> bucket_50ms{0};
    std::atomic<int64_t> bucket_100ms{0};
    std::atomic<int64_t> bucket_500ms{0};
    std::atomic<int64_t> bucket_inf{0};
};

class MetricsCollector
{
public:
    static MetricsCollector &instance()
    {
        static MetricsCollector mc;
        return mc;
    }

    void setInflightSource(const std::atomic<int>* src) { inflightSrc_ = src; }

    void record(const std::string &endpoint, const std::string &method, int64_t latency_us, bool is_error)
    {
        auto &m = getOrCreate(method + ":" + endpoint);
        m.total.fetch_add(1, std::memory_order_relaxed);
        if (is_error)
            m.errors.fetch_add(1, std::memory_order_relaxed);

        m.latency_us_sum.fetch_add(latency_us, std::memory_order_relaxed);

        // Update min (only if new value is smaller)
        int64_t oldMin = m.latency_us_min.load(std::memory_order_relaxed);
        while (latency_us < oldMin &&
               !m.latency_us_min.compare_exchange_weak(oldMin, latency_us,
                   std::memory_order_relaxed))
            ;

        // Update max
        int64_t oldMax = m.latency_us_max.load(std::memory_order_relaxed);
        while (latency_us > oldMax &&
               !m.latency_us_max.compare_exchange_weak(oldMax, latency_us,
                   std::memory_order_relaxed))
            ;

        // Latency buckets
        auto &bucket = (latency_us < 10000)    ? m.bucket_10ms :
                       (latency_us < 50000)    ? m.bucket_50ms :
                       (latency_us < 100000)   ? m.bucket_100ms :
                       (latency_us < 500000)   ? m.bucket_500ms :
                                                 m.bucket_inf;
        bucket.fetch_add(1, std::memory_order_relaxed);
    }

    nlohmann::json toJson() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        nlohmann::json j;
        j["uptime_seconds"] = uptimeSeconds();

        nlohmann::json eps = nlohmann::json::object();
        for (auto &[name, m] : endpoints_)
        {
            nlohmann::json e;
            e["total"]  = m.total.load(std::memory_order_relaxed);
            e["errors"] = m.errors.load(std::memory_order_relaxed);
            int64_t total = m.total.load(std::memory_order_relaxed);
            if (total > 0)
            {
                e["avg_latency_us"] = m.latency_us_sum.load(std::memory_order_relaxed) / total;
            }
            else
            {
                e["avg_latency_us"] = 0;
            }
            int64_t minVal = m.latency_us_min.load(std::memory_order_relaxed);
            e["latency_us_min"] = (minVal == INT64_MAX) ? 0 : minVal;
            e["latency_us_max"] = m.latency_us_max.load(std::memory_order_relaxed);
            e["buckets"] = {
                {"<10ms",   m.bucket_10ms.load(std::memory_order_relaxed)},
                {"<50ms",   m.bucket_50ms.load(std::memory_order_relaxed)},
                {"<100ms",  m.bucket_100ms.load(std::memory_order_relaxed)},
                {"<500ms",  m.bucket_500ms.load(std::memory_order_relaxed)},
                {">=500ms", m.bucket_inf.load(std::memory_order_relaxed)}
            };
            eps[name] = e;
        }
        j["endpoints"] = eps;
        return j;
    }

    std::string toPrometheus() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::string out;

        auto addMetric = [&](const std::string &name, const std::string &help,
                             const std::string &type, const std::string &body) {
            out += "# HELP " + name + " " + help + "\n";
            out += "# TYPE " + name + " " + type + "\n";
            out += body;
        };

        // Counters: total + errors
        std::string totalBody, errorsBody;
        for (auto &[key, m] : endpoints_)
        {
            auto colonPos = key.find(':');
            std::string method = (colonPos != std::string::npos) ? key.substr(0, colonPos) : "UNKNOWN";
            std::string path   = (colonPos != std::string::npos) ? key.substr(colonPos + 1) : key;
            std::string labels = "{endpoint=\"" + path + "\",method=\"" + method + "\"}";

            int64_t t = m.total.load(std::memory_order_relaxed);
            int64_t e = m.errors.load(std::memory_order_relaxed);
            totalBody  += "http_requests_total"  + labels + " " + std::to_string(t) + "\n";
            errorsBody += "http_request_errors_total" + labels + " " + std::to_string(e) + "\n";
        }
        addMetric("http_requests_total", "Total number of HTTP requests", "counter", totalBody);
        addMetric("http_request_errors_total", "Total number of HTTP request errors", "counter", errorsBody);

        // Histogram: latency buckets
        std::string histBody;
        for (auto &[key, m] : endpoints_)
        {
            auto colonPos = key.find(':');
            std::string method = (colonPos != std::string::npos) ? key.substr(0, colonPos) : "UNKNOWN";
            std::string path   = (colonPos != std::string::npos) ? key.substr(colonPos + 1) : key;
            std::string labels = "{endpoint=\"" + path + "\",method=\"" + method + "\"}";

            int64_t b10ms  = m.bucket_10ms.load(std::memory_order_relaxed);
            int64_t b50ms  = m.bucket_50ms.load(std::memory_order_relaxed);
            int64_t b100ms = m.bucket_100ms.load(std::memory_order_relaxed);
            int64_t b500ms = m.bucket_500ms.load(std::memory_order_relaxed);
            int64_t bInf   = m.bucket_inf.load(std::memory_order_relaxed);
            int64_t total  = b10ms + b50ms + b100ms + b500ms + bInf;
            int64_t sum    = m.latency_us_sum.load(std::memory_order_relaxed);

            histBody += "http_request_duration_microseconds_bucket" + labels + "{le=\"10000\"} " + std::to_string(b10ms) + "\n";
            histBody += "http_request_duration_microseconds_bucket" + labels + "{le=\"50000\"} " + std::to_string(b10ms + b50ms) + "\n";
            histBody += "http_request_duration_microseconds_bucket" + labels + "{le=\"100000\"} " + std::to_string(b10ms + b50ms + b100ms) + "\n";
            histBody += "http_request_duration_microseconds_bucket" + labels + "{le=\"500000\"} " + std::to_string(b10ms + b50ms + b100ms + b500ms) + "\n";
            histBody += "http_request_duration_microseconds_bucket" + labels + "{le=\"+Inf\"} " + std::to_string(total) + "\n";
            histBody += "http_request_duration_microseconds_count" + labels + " " + std::to_string(total) + "\n";
            histBody += "http_request_duration_microseconds_sum" + labels + " " + std::to_string(sum) + "\n";
        }
        addMetric("http_request_duration_microseconds", "HTTP request latency in microseconds", "histogram", histBody);

        // Inflight gauge
        if (inflightSrc_)
        {
            std::string inflightVal = std::to_string(inflightSrc_->load(std::memory_order_relaxed));
            addMetric("http_requests_inflight", "Currently in-flight HTTP requests", "gauge",
                      "http_requests_inflight " + inflightVal + "\n");
        }

        // Uptime gauge
        int64_t uptime = uptimeSeconds();
        addMetric("process_uptime_seconds", "Process uptime in seconds", "gauge",
                  "process_uptime_seconds " + std::to_string(uptime) + "\n");

        return out;
    }

private:
    MetricsCollector() : startTime_(muduo::Timestamp::now()) {}

    int64_t uptimeSeconds() const
    {
        return (muduo::Timestamp::now().microSecondsSinceEpoch() -
                startTime_.microSecondsSinceEpoch()) / 1000000;
    }

    EndpointMetrics &getOrCreate(const std::string &name)
    {
        // Fast path: read with shared lock
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            auto it = endpoints_.find(name);
            if (it != endpoints_.end())
                return it->second;
        }
        // Slow path: create under exclusive lock
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return endpoints_[name];
    }

    muduo::Timestamp startTime_;
    std::unordered_map<std::string, EndpointMetrics> endpoints_;
    mutable std::shared_mutex mutex_;
    const std::atomic<int>* inflightSrc_ = nullptr;
};
