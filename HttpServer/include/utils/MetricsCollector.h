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

struct ModelMetrics {
    std::atomic<int64_t> count{0};
    std::atomic<int64_t> latency_us_sum{0};
    std::atomic<int64_t> latency_us_min{INT64_MAX};
    std::atomic<int64_t> latency_us_max{0};
    std::atomic<int64_t> bucket_1ms{0};
    std::atomic<int64_t> bucket_5ms{0};
    std::atomic<int64_t> bucket_10ms{0};
    std::atomic<int64_t> bucket_25ms{0};
    std::atomic<int64_t> bucket_50ms{0};
    std::atomic<int64_t> bucket_100ms{0};
    std::atomic<int64_t> bucket_250ms{0};
    std::atomic<int64_t> bucket_500ms{0};
    std::atomic<int64_t> bucket_1s{0};
    std::atomic<int64_t> bucket_2_5s{0};
    std::atomic<int64_t> bucket_inf{0};
};

/// Per-phase latency within a single inference pipeline call.
/// key = "model:task:phase" (e.g. "resnet50:classification:preprocess")
struct PhaseMetrics {
    std::atomic<int64_t> count{0};
    std::atomic<int64_t> latency_us_sum{0};
    std::atomic<int64_t> latency_us_min{INT64_MAX};
    std::atomic<int64_t> latency_us_max{0};
};

/// Batch efficiency metrics per model.
/// key = "modelName" (e.g. "vision_model_onnx")
struct BatchMetrics {
    std::atomic<int64_t> batches_total{0};
    std::atomic<int64_t> requests_total{0};
    std::atomic<int64_t> batch_size_sum{0};
    std::atomic<int64_t> queue_wait_us_sum{0};
    std::atomic<int64_t> queue_wait_us_max{0};
    std::atomic<int64_t> queue_wait_us_min{INT64_MAX};
};

class MetricsCollector
{
public:
    static MetricsCollector &instance()
    {
        static MetricsCollector mc;
        return mc;
    }

    void recordPhaseLatency(const std::string& model, const std::string& taskType,
                            const std::string& phase, int64_t latency_us)
    {
        std::string key = model + ":" + taskType + ":" + phase;
        auto &m = getOrCreatePhase(key);
        m.count.fetch_add(1, std::memory_order_relaxed);
        m.latency_us_sum.fetch_add(latency_us, std::memory_order_relaxed);
        int64_t oldMin = m.latency_us_min.load(std::memory_order_relaxed);
        while (latency_us < oldMin &&
               !m.latency_us_min.compare_exchange_weak(oldMin, latency_us, std::memory_order_relaxed))
            ;
        int64_t oldMax = m.latency_us_max.load(std::memory_order_relaxed);
        while (latency_us > oldMax &&
               !m.latency_us_max.compare_exchange_weak(oldMax, latency_us, std::memory_order_relaxed))
            ;
    }

    void recordBatchMetrics(const std::string& model,
                            int batchSize,
                            int64_t queueWaitUs)
    {
        auto &m = getOrCreateBatch(model);
        m.batches_total.fetch_add(1, std::memory_order_relaxed);
        m.requests_total.fetch_add(batchSize, std::memory_order_relaxed);
        m.batch_size_sum.fetch_add(batchSize, std::memory_order_relaxed);
        m.queue_wait_us_sum.fetch_add(queueWaitUs, std::memory_order_relaxed);

        int64_t oldMin = m.queue_wait_us_min.load(std::memory_order_relaxed);
        while (queueWaitUs < oldMin &&
               !m.queue_wait_us_min.compare_exchange_weak(oldMin, queueWaitUs, std::memory_order_relaxed))
            ;
        int64_t oldMax = m.queue_wait_us_max.load(std::memory_order_relaxed);
        while (queueWaitUs > oldMax &&
               !m.queue_wait_us_max.compare_exchange_weak(oldMax, queueWaitUs, std::memory_order_relaxed))
            ;
    }

    void setInflightSource(const std::atomic<int>* src) { inflightSrc_ = src; }

    // Model-level inference latency (separate from HTTP-level metrics)
    void recordModelLatency(const std::string& model, const std::string& taskType,
                            int64_t latency_us, int batchSize = 1)
    {
        std::string key = model + ":" + taskType;
        auto &m = getOrCreateModel(key);
        m.count.fetch_add(batchSize, std::memory_order_relaxed);
        m.latency_us_sum.fetch_add(latency_us, std::memory_order_relaxed);

        int64_t oldMin = m.latency_us_min.load(std::memory_order_relaxed);
        while (latency_us < oldMin &&
               !m.latency_us_min.compare_exchange_weak(oldMin, latency_us, std::memory_order_relaxed))
            ;
        int64_t oldMax = m.latency_us_max.load(std::memory_order_relaxed);
        while (latency_us > oldMax &&
               !m.latency_us_max.compare_exchange_weak(oldMax, latency_us, std::memory_order_relaxed))
            ;

        // Inference-specific buckets: 1ms, 5ms, 10ms, 25ms, 50ms, 100ms, 250ms, 500ms, 1s, 2.5s
        auto &b = (latency_us < 1000)     ? m.bucket_1ms :
                  (latency_us < 5000)     ? m.bucket_5ms :
                  (latency_us < 10000)    ? m.bucket_10ms :
                  (latency_us < 25000)    ? m.bucket_25ms :
                  (latency_us < 50000)    ? m.bucket_50ms :
                  (latency_us < 100000)   ? m.bucket_100ms :
                  (latency_us < 250000)   ? m.bucket_250ms :
                  (latency_us < 500000)   ? m.bucket_500ms :
                  (latency_us < 1000000)  ? m.bucket_1s :
                  (latency_us < 2500000)  ? m.bucket_2_5s :
                                            m.bucket_inf;
        b.fetch_add(batchSize, std::memory_order_relaxed);
    }

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

        // Model inference metrics
        nlohmann::json mods = nlohmann::json::object();
        for (auto &[key, m] : modelMetrics_)
        {
            auto colonPos = key.find(':');
            std::string model = (colonPos != std::string::npos) ? key.substr(0, colonPos) : key;
            std::string task  = (colonPos != std::string::npos) ? key.substr(colonPos + 1) : "";
            nlohmann::json mm;
            mm["model"] = model;
            mm["task"] = task;
            mm["count"] = m.count.load(std::memory_order_relaxed);
            int64_t c = m.count.load(std::memory_order_relaxed);
            mm["avg_latency_us"] = c > 0 ? m.latency_us_sum.load(std::memory_order_relaxed) / c : 0;
            int64_t minVal = m.latency_us_min.load(std::memory_order_relaxed);
            mm["latency_us_min"] = (minVal == INT64_MAX) ? 0 : minVal;
            mm["latency_us_max"] = m.latency_us_max.load(std::memory_order_relaxed);
            mm["buckets"] = {
                {"<1ms",    m.bucket_1ms.load(std::memory_order_relaxed)},
                {"<5ms",    m.bucket_5ms.load(std::memory_order_relaxed)},
                {"<10ms",   m.bucket_10ms.load(std::memory_order_relaxed)},
                {"<25ms",   m.bucket_25ms.load(std::memory_order_relaxed)},
                {"<50ms",   m.bucket_50ms.load(std::memory_order_relaxed)},
                {"<100ms",  m.bucket_100ms.load(std::memory_order_relaxed)},
                {"<250ms",  m.bucket_250ms.load(std::memory_order_relaxed)},
                {"<500ms",  m.bucket_500ms.load(std::memory_order_relaxed)},
                {"<1s",     m.bucket_1s.load(std::memory_order_relaxed)},
                {"<2.5s",   m.bucket_2_5s.load(std::memory_order_relaxed)},
                {">=2.5s",  m.bucket_inf.load(std::memory_order_relaxed)}
            };
            mods[key] = mm;
        }
        j["model_inference"] = mods;

        // Phase-level latency breakdown
        nlohmann::json phases = nlohmann::json::object();
        for (auto &[key, m] : phaseMetrics_)
        {
            // key = "model:task:phase"
            auto first  = key.find(':');
            auto second = key.rfind(':');
            std::string model = key.substr(0, first);
            std::string task  = key.substr(first + 1, second - first - 1);
            std::string phase = key.substr(second + 1);
            nlohmann::json pm;
            pm["model"] = model;
            pm["task"] = task;
            pm["phase"] = phase;
            pm["count"] = m.count.load(std::memory_order_relaxed);
            int64_t c = m.count.load(std::memory_order_relaxed);
            pm["avg_latency_us"] = c > 0 ? m.latency_us_sum.load(std::memory_order_relaxed) / c : 0;
            int64_t minVal = m.latency_us_min.load(std::memory_order_relaxed);
            pm["latency_us_min"] = (minVal == INT64_MAX) ? 0 : minVal;
            pm["latency_us_max"] = m.latency_us_max.load(std::memory_order_relaxed);
            phases[key] = pm;
        }
        j["pipeline_phases"] = phases;

        // Batch efficiency metrics
        nlohmann::json bat = nlohmann::json::object();
        for (auto &[model, m] : batchMetrics_)
        {
            nlohmann::json bm;
            bm["model"] = model;
            bm["batches_total"]  = m.batches_total.load(std::memory_order_relaxed);
            bm["requests_total"] = m.requests_total.load(std::memory_order_relaxed);
            int64_t nb = m.batches_total.load(std::memory_order_relaxed);
            bm["avg_batch_size"] = nb > 0
                ? static_cast<double>(m.batch_size_sum.load(std::memory_order_relaxed)) / nb
                : 0.0;
            int64_t nr = m.requests_total.load(std::memory_order_relaxed);
            bm["avg_queue_wait_us"] = nr > 0
                ? m.queue_wait_us_sum.load(std::memory_order_relaxed) / nr
                : 0;
            int64_t minVal = m.queue_wait_us_min.load(std::memory_order_relaxed);
            bm["queue_wait_us_min"] = (minVal == INT64_MAX) ? 0 : minVal;
            bm["queue_wait_us_max"] = m.queue_wait_us_max.load(std::memory_order_relaxed);
            bat[model] = bm;
        }
        j["batching"] = bat;

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

    ModelMetrics &getOrCreateModel(const std::string &name)
    {
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            auto it = modelMetrics_.find(name);
            if (it != modelMetrics_.end())
                return it->second;
        }
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return modelMetrics_[name];
    }

    PhaseMetrics &getOrCreatePhase(const std::string &name)
    {
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            auto it = phaseMetrics_.find(name);
            if (it != phaseMetrics_.end())
                return it->second;
        }
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return phaseMetrics_[name];
    }

    BatchMetrics &getOrCreateBatch(const std::string &name)
    {
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            auto it = batchMetrics_.find(name);
            if (it != batchMetrics_.end())
                return it->second;
        }
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return batchMetrics_[name];
    }

    muduo::Timestamp startTime_;
    std::unordered_map<std::string, EndpointMetrics> endpoints_;
    std::unordered_map<std::string, ModelMetrics> modelMetrics_;
    std::unordered_map<std::string, PhaseMetrics> phaseMetrics_;
    std::unordered_map<std::string, BatchMetrics> batchMetrics_;
    mutable std::shared_mutex mutex_;
    const std::atomic<int>* inflightSrc_ = nullptr;
};
