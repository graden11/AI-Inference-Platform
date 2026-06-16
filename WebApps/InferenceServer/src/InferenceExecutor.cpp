#include "../include/InferenceExecutor.h"

InferenceExecutor::InferenceExecutor(int maxConcurrency, int queueMultiplier)
    : pool_(maxConcurrency),
      maxTotal_(maxConcurrency * (1 + queueMultiplier))
{
}

InferenceExecutor::~InferenceExecutor() = default;

std::future<void> InferenceExecutor::submit(std::function<void()> task)
{
    // ── Back-pressure: block when total inflight (running + queued) would
    //     exceed the limit.  This lets the scheduler slow down naturally
    //     instead of piling up unbounded work in the ThreadPool queue. ──
    {
        std::unique_lock<std::mutex> lock(gateMutex_);
        gateCv_.wait(lock, [this]() {
            return (inflight_.load(std::memory_order_relaxed) +
                    queued_.load(std::memory_order_relaxed)) < maxTotal_;
        });
    }

    queued_.fetch_add(1, std::memory_order_relaxed);

    try
    {
        return pool_.enqueue([this, t = std::move(task)]() {
            // Transition queued → running
            queued_.fetch_sub(1, std::memory_order_relaxed);
            inflight_.fetch_add(1, std::memory_order_relaxed);

            t();

            inflight_.fetch_sub(1, std::memory_order_relaxed);
            // Wake one blocked submitter
            gateCv_.notify_one();
        });
    }
    catch (...)
    {
        // enqueue failed — roll back the queued_ increment so we don't
        // permanently leak a slot
        queued_.fetch_sub(1, std::memory_order_relaxed);
        gateCv_.notify_one();
        throw;
    }
}
