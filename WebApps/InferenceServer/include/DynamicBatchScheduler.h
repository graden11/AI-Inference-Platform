#pragma once

#include "InferenceExecutor.h"
#include "ModelFactory.h"
#include "RequestBatcher.h"   // for RequestBatcher::Item
#include "RequestSlotPool.h"

#include "../../../HttpServer/include/utils/ConfigLoader.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace http { struct PerfTrace; }

// ─────────────────────────────────────────────────────────────────────────────
// SchedulingKey — modelName:height:width:channels
// ─────────────────────────────────────────────────────────────────────────────
struct SchedulingKey
{
    std::string modelName;
    int height = 0;
    int width  = 0;
    int channels = 0;

    bool operator==(const SchedulingKey& other) const
    {
        return modelName == other.modelName
            && height   == other.height
            && width    == other.width
            && channels == other.channels;
    }
};

namespace std {
template<>
struct hash<SchedulingKey>
{
    size_t operator()(const SchedulingKey& k) const noexcept
    {
        size_t h = std::hash<std::string>{}(k.modelName);
        h ^= std::hash<int>{}(k.height)   + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(k.width)    + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(k.channels) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};
} // namespace std

// ─────────────────────────────────────────────────────────────────────────────
// BatchPlan
// ─────────────────────────────────────────────────────────────────────────────
struct BatchPlan
{
    SchedulingKey key;
    std::vector<RequestBatcher::Item> items;

    enum class Reason { PREFERRED, TIMEOUT, MAX_BATCH, SHUTDOWN };
    Reason reason = Reason::PREFERRED;
};

// ─────────────────────────────────────────────────────────────────────────────
// ModelQueue — per-key queue holding pending items + scheduling parameters
// ─────────────────────────────────────────────────────────────────────────────
struct ModelQueue
{
    std::deque<RequestBatcher::Item> items;

    int maxBatchSize = 16;
    std::vector<int> preferredBatchSizes;  // sorted ascending
    int maxQueueDelayUs = 5000;

    // TODO: preserve_ordering — if needed, could be implemented via a
    // per-queue response ordering queue that buffers completed futures and
    // fulfills promises in FIFO order.
};

// ─────────────────────────────────────────────────────────────────────────────
// DynamicBatchScheduler
// ─────────────────────────────────────────────────────────────────────────────
class DynamicBatchScheduler
{
public:
    DynamicBatchScheduler(ModelFactory* factory,
                          const BatchingConfig& config,
                          InferenceExecutor* gpuExecutor,
                          InferenceExecutor* cpuExecutor);
    ~DynamicBatchScheduler();

    DynamicBatchScheduler(const DynamicBatchScheduler&) = delete;
    DynamicBatchScheduler& operator=(const DynamicBatchScheduler&) = delete;

    // ── Compatible with RequestBatcher::submit() ──
    std::future<std::string> submit(std::string modelName,
                                    std::shared_ptr<RequestSlot> slot,
                                    int inputW = 0, int inputH = 0, int inputC = 0);

    void start();
    void stop();

    struct Snapshot
    {
        int64_t totalQueued = 0;
        int64_t dropped     = 0;
        int64_t batches     = 0;
        int64_t requests    = 0;
    };
    Snapshot snapshot() const;

private:
    SchedulingKey makeKey(const std::string& modelName,
                          int w, int h, int c) const;

    void resolveConfig(const std::string& modelName,
                       int w, int h, int c,
                       ModelQueue& mq);

    /// Decide how many items to take from the queue, or return empty plan.
    BatchPlan decideBatch(ModelQueue& mq, const SchedulingKey& key);

    void executeBatch(BatchPlan plan);
    void schedulerLoop();

    ModelFactory* factory_;
    BatchingConfig config_;
    InferenceExecutor* gpuExecutor_;
    InferenceExecutor* cpuExecutor_;

    std::unordered_map<SchedulingKey, ModelQueue> queues_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> running_{false};

    // ── Metrics counters ──
    std::atomic<int64_t> totalQueued_{0};
    std::atomic<int64_t> dropped_{0};
    std::atomic<int64_t> batches_{0};
    std::atomic<int64_t> requests_{0};

    std::vector<std::future<void>> pendingFutures_;
};
