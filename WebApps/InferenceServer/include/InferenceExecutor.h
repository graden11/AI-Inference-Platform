#pragma once

#include "ThreadPool.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>

/// Bounded executor for inference batch dispatch.
///
/// Wraps a ThreadPool to provide a fixed-concurrency execution context with
/// back-pressure.  GPU models use concurrency=1 (TRTBackend serialises
/// internally with gpu_mutex_); CPU models use concurrency=2~4.
///
/// Key difference from raw std::async / unbounded ThreadPool:
///   - Tracks total inflight tasks (running + queued in ThreadPool).
///   - Blocks submit() when the total reaches maxConcurrency * queueMultiplier,
///     applying back-pressure to the scheduler.  This prevents unbounded
///     queue growth under overload.
class InferenceExecutor
{
public:
    /// @param maxConcurrency  Number of worker threads (1 for GPU, 2-4 for CPU).
    /// @param queueMultiplier Max queued tasks = maxConcurrency * queueMultiplier
    ///                        (default 4, so GPU can queue up to 4 batches).
    explicit InferenceExecutor(int maxConcurrency, int queueMultiplier = 4);
    ~InferenceExecutor();

    InferenceExecutor(const InferenceExecutor&) = delete;
    InferenceExecutor& operator=(const InferenceExecutor&) = delete;

    /// Submit a fire-and-forget task.  Returns a future that becomes ready
    /// when the task completes.
    ///
    /// Blocks if (inflight + queued) >= maxConcurrency * (1 + queueMultiplier).
    /// This is intentional back-pressure from the scheduler.
    std::future<void> submit(std::function<void()> task);

    /// Total tasks submitted but not yet completed (running + queued).
    int inflightCount() const {
        return inflight_.load(std::memory_order_relaxed)
             + queued_.load(std::memory_order_relaxed);
    }

    /// Number of tasks queued in the underlying ThreadPool but not yet executing.
    int queueDepth() const { return queued_.load(std::memory_order_relaxed); }

    int maxConcurrency() const { return pool_.threadCount(); }

private:
    ThreadPool pool_;
    std::atomic<int> inflight_{0};
    std::atomic<int> queued_{0};

    int maxTotal_;   // maxConcurrency * (1 + queueMultiplier)
    std::mutex gateMutex_;
    std::condition_variable gateCv_;
};
