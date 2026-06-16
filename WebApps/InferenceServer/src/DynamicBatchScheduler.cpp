#include "../include/DynamicBatchScheduler.h"
#include "../include/InferenceEngine.h"
#include "../include/RequestBatcher.h"    // for RequestBatcher::Item

#include "../../../HttpServer/include/http/HttpResponse.h"  // PerfTrace
#include "../../../HttpServer/include/utils/MetricsCollector.h"

#include <muduo/base/Logging.h>

#include <algorithm>
#include <chrono>

using namespace std::chrono;

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

DynamicBatchScheduler::DynamicBatchScheduler(ModelFactory* factory,
                                             const BatchingConfig& config,
                                             InferenceExecutor* gpuExecutor,
                                             InferenceExecutor* cpuExecutor)
    : factory_(factory),
      config_(config),
      gpuExecutor_(gpuExecutor),
      cpuExecutor_(cpuExecutor)
{
}

DynamicBatchScheduler::~DynamicBatchScheduler()
{
    stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// start / stop
// ─────────────────────────────────────────────────────────────────────────────

void DynamicBatchScheduler::start()
{
    running_.store(true);
    worker_ = std::thread(&DynamicBatchScheduler::schedulerLoop, this);

    LOG_INFO << "DynamicBatchScheduler started maxBatchSize=" << config_.max_batch_size
             << " maxQueueDelayUs=" << config_.max_queue_delay_us
             << " maxQueueSize=" << config_.max_queue_size
             << " preferredSizes=" << [&]() {
                 std::string s;
                 for (auto v : config_.preferred_batch_sizes) {
                     if (!s.empty()) s += ",";
                     s += std::to_string(v);
                 }
                 return s;
             }()
             << " gpuExecConcurrency=" << (gpuExecutor_ ? gpuExecutor_->maxConcurrency() : 0)
             << " cpuExecConcurrency=" << (cpuExecutor_ ? cpuExecutor_->maxConcurrency() : 0);
}

void DynamicBatchScheduler::stop()
{
    running_.store(false);
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();

    // Drain pending futures (avoid dangling references)
    for (auto& f : pendingFutures_)
        if (f.valid()) f.wait();
    pendingFutures_.clear();

    // Reject all remaining items
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [key, mq] : queues_)
        {
            for (auto& item : mq.items)
            {
                item.promise->set_value(
                    R"({"status":"error","message":"request cancelled: server shutting down"})");
            }
            mq.items.clear();
        }
        queues_.clear();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Scheduling key
// ─────────────────────────────────────────────────────────────────────────────

SchedulingKey DynamicBatchScheduler::makeKey(const std::string& modelName,
                                              int w, int h, int c) const
{
    // Keep full modelName including version suffix (e.g. "model:v1").
    // Different versions map to different queues → cannot be merged into the
    // same batch, which is correct since they resolve to different engines.
    return SchedulingKey{modelName, (h > 0 ? h : 224), (w > 0 ? w : 224), (c > 0 ? c : 3)};
}

// ─────────────────────────────────────────────────────────────────────────────
// Config resolution (global + per-model overrides)
// ─────────────────────────────────────────────────────────────────────────────

void DynamicBatchScheduler::resolveConfig(const std::string& modelName,
                                           int w, int h, int c,
                                           ModelQueue& mq)
{
    mq.maxBatchSize      = config_.max_batch_size;
    mq.preferredBatchSizes = config_.preferred_batch_sizes;
    mq.maxQueueDelayUs   = config_.max_queue_delay_us;

    auto key = makeKey(modelName, w, h, c);

    // Per-model overrides are keyed by bare model name (no version suffix).
    // Try the full name first, then fall back to the bare name.
    auto it = config_.models.find(key.modelName);
    if (it == config_.models.end())
    {
        std::string bareName = key.modelName;
        auto colon = bareName.rfind(':');
        if (colon != std::string::npos)
        {
            bareName = bareName.substr(0, colon);
            it = config_.models.find(bareName);
        }
    }

    if (it != config_.models.end())
    {
        auto& pmc = it->second;
        if (pmc.max_batch_size > 0)
            mq.maxBatchSize = pmc.max_batch_size;
        if (!pmc.preferred_batch_sizes.empty())
            mq.preferredBatchSizes = pmc.preferred_batch_sizes;
        if (pmc.max_queue_delay_us > 0)
            mq.maxQueueDelayUs = pmc.max_queue_delay_us;
    }

    std::sort(mq.preferredBatchSizes.begin(), mq.preferredBatchSizes.end());
    if (mq.preferredBatchSizes.empty())
        mq.preferredBatchSizes.push_back(mq.maxBatchSize);

    mq.preferredBatchSizes.erase(
        std::remove_if(mq.preferredBatchSizes.begin(), mq.preferredBatchSizes.end(),
                       [&](int s) { return s > mq.maxBatchSize; }),
        mq.preferredBatchSizes.end());
}

// ─────────────────────────────────────────────────────────────────────────────
// submit (public API — same signature as RequestBatcher)
// ─────────────────────────────────────────────────────────────────────────────

std::future<std::string> DynamicBatchScheduler::submit(std::string modelName,
                                                        std::shared_ptr<RequestSlot> slot,
                                                        int inputW, int inputH, int inputC)
{
    auto promise = std::make_shared<std::promise<std::string>>();
    auto future = promise->get_future();

    auto key = makeKey(modelName, inputW, inputH, inputC);

    // B0: enqueue time
    if (slot && slot->perfTrace)
        slot->perfTrace->b0_enqueue = slot->perfTrace->nowUs();

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = queues_.find(key);
        if (it == queues_.end())
        {
            ModelQueue mq;
            resolveConfig(modelName, inputW, inputH, inputC, mq);
            auto [insertedIt, _] = queues_.emplace(key, std::move(mq));
            it = insertedIt;
        }

        auto& mq = it->second;

        // Queue capacity check
        int maxQ = config_.max_queue_size > 0 ? config_.max_queue_size : 1024;
        if (static_cast<int>(mq.items.size()) >= maxQ)
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            promise->set_value(
                R"({"status":"error","message":"server busy: queue full, retry later"})");
            return future;
        }

        RequestBatcher::Item item;
        item.modelName     = std::move(modelName);
        item.inputWidth    = inputW;
        item.inputHeight   = inputH;
        item.inputChannels = inputC;
        item.slot          = std::move(slot);
        item.promise       = std::move(promise);
        item.enqueueTime   = steady_clock::now();
        if (item.slot)
            item.perfTrace = item.slot->perfTrace;

        mq.items.push_back(std::move(item));
        totalQueued_.fetch_add(1, std::memory_order_relaxed);
    }

    cv_.notify_one();
    return future;
}

// ─────────────────────────────────────────────────────────────────────────────
// decideBatch — core scheduling decision
//
// Decision order:
//   1. queue.size >= maxBatchSize   → take maxBatchSize,   reason=MAX_BATCH
//   2. queue.size >= any preferred  → take largest pref ≤ n, reason=PREFERRED
//   3. oldest item waited ≥ delay   → take what we have,   reason=TIMEOUT
//   4. otherwise                    → WAIT (return empty plan)
// ─────────────────────────────────────────────────────────────────────────────

BatchPlan DynamicBatchScheduler::decideBatch(ModelQueue& mq, const SchedulingKey& key)
{
    BatchPlan plan;
    plan.key = key;

    int n = static_cast<int>(mq.items.size());
    if (n == 0)
        return plan;

    auto now = steady_clock::now();
    int64_t oldestWaitUs = duration_cast<microseconds>(
        now - mq.items.front().enqueueTime).count();

    // 1. Queue >= maxBatchSize
    if (n >= mq.maxBatchSize)
    {
        for (int i = 0; i < mq.maxBatchSize; ++i)
        {
            plan.items.push_back(std::move(mq.items.front()));
            mq.items.pop_front();
        }
        plan.reason = BatchPlan::Reason::MAX_BATCH;
        return plan;
    }

    // 2. Queue >= min(preferred) → take largest preferred ≤ n
    int minPref = mq.preferredBatchSizes.empty() ? mq.maxBatchSize
                                                 : mq.preferredBatchSizes.front();
    if (n >= minPref)
    {
        int take = minPref;
        for (int s : mq.preferredBatchSizes)
        {
            if (s <= n)
                take = s;
            else
                break;
        }
        for (int i = 0; i < take; ++i)
        {
            plan.items.push_back(std::move(mq.items.front()));
            mq.items.pop_front();
        }
        plan.reason = BatchPlan::Reason::PREFERRED;
        return plan;
    }

    // 3. Oldest request waited >= maxQueueDelayUs → timeout
    if (oldestWaitUs >= mq.maxQueueDelayUs)
    {
        int take = std::min(n, mq.maxBatchSize);
        for (int i = 0; i < take; ++i)
        {
            plan.items.push_back(std::move(mq.items.front()));
            mq.items.pop_front();
        }
        plan.reason = BatchPlan::Reason::TIMEOUT;
        return plan;
    }

    // 4. Still in waiting window — return empty plan
    return plan;
}

// ─────────────────────────────────────────────────────────────────────────────
// executeBatch — fire inference and set results
// ─────────────────────────────────────────────────────────────────────────────

void DynamicBatchScheduler::executeBatch(BatchPlan plan)
{
    if (plan.items.empty())
        return;

    int batchSize = static_cast<int>(plan.items.size());

    // Safety: verify all items target the same model.  Different versions of
    // the same base model must NOT be merged because they resolve to different
    // engines.  If a mismatch is detected, split by modelName and recurse.
    {
        const auto& firstName = plan.items[0].modelName;
        for (size_t i = 1; i < plan.items.size(); ++i)
        {
            if (plan.items[i].modelName != firstName)
            {
                LOG_ERROR << "executeBatch: modelName mismatch in batch '"
                          << firstName << "' vs '" << plan.items[i].modelName
                          << "' — splitting batch";
                // Group by modelName
                std::unordered_map<std::string, BatchPlan> splits;
                for (auto& it : plan.items)
                {
                    auto& sub = splits[it.modelName];
                    sub.key = plan.key;
                    sub.key.modelName = it.modelName;
                    sub.items.push_back(std::move(it));
                }
                for (auto& [name, subPlan] : splits)
                {
                    subPlan.reason = plan.reason;
                    executeBatch(std::move(subPlan));
                }
                return;
            }
        }
    }

    // B2: batch-collected time
    for (auto& it : plan.items)
        if (it.perfTrace)
            it.perfTrace->b2_batch_collected = it.perfTrace->nowUs();

    struct TaskItem
    {
        std::shared_ptr<std::promise<std::string>> promise;
        std::shared_ptr<RequestSlot> slot;
        std::shared_ptr<http::PerfTrace> perfTrace;
        int64_t enqueueUs;
    };
    auto tasks = std::make_shared<std::vector<TaskItem>>();
    tasks->reserve(batchSize);
    for (auto& it : plan.items)
    {
        tasks->push_back({
            std::move(it.promise),
            std::move(it.slot),
            std::move(it.perfTrace),
            duration_cast<microseconds>(
                it.enqueueTime.time_since_epoch()).count()
        });
    }

    std::string modelName = plan.items[0].modelName;

    // B3: group dispatch
    for (auto& t : *tasks)
        if (t.perfTrace)
            t.perfTrace->b3_group_dispatch = t.perfTrace->nowUs();

    // Collect image bytes
    auto images = std::make_shared<std::vector<std::vector<uint8_t>>>();
    images->reserve(tasks->size());
    for (auto& t : *tasks)
    {
        if (t.slot)
            images->push_back(std::move(t.slot->imageBytes));
        else
            images->emplace_back();
    }

    // Choose executor: GPU or CPU based on model name
    bool isGpu = (modelName.find("_trt") != std::string::npos)
              || (modelName.find("_TRT") != std::string::npos);
    InferenceExecutor* exec = (isGpu && gpuExecutor_) ? gpuExecutor_ : cpuExecutor_;
    if (!exec) exec = gpuExecutor_;

    // Record metrics
    {
        std::string reasonStr;
        switch (plan.reason)
        {
            case BatchPlan::Reason::PREFERRED: reasonStr = "preferred";  break;
            case BatchPlan::Reason::TIMEOUT:   reasonStr = "timeout";    break;
            case BatchPlan::Reason::MAX_BATCH: reasonStr = "max_batch";  break;
            case BatchPlan::Reason::SHUTDOWN:  reasonStr = "shutdown";   break;
        }
        auto& mc = MetricsCollector::instance();

        int64_t maxWait = 0;
        auto now = steady_clock::now();
        for (auto& t : *tasks)
        {
            int64_t w = duration_cast<microseconds>(
                now.time_since_epoch()).count() - t.enqueueUs;
            if (w > maxWait) maxWait = w;
        }
        mc.recordBatchMetrics(modelName, batchSize, maxWait);
        mc.recordBatchDispatch(modelName, reasonStr, batchSize);
    }

    batches_.fetch_add(1, std::memory_order_relaxed);
    requests_.fetch_add(batchSize, std::memory_order_relaxed);

    // Dispatch to executor — keep a backup shared_ptr in case submit() throws.
    // If submit() fails, we must set error promises on every task so no client
    // hangs forever on future.get().
    auto tasksBackup = tasks;  // retain access if submit() throws and moves tasks
    try
    {
        pendingFutures_.push_back(
            exec->submit([this, tasks = std::move(tasks),
                           images = std::move(images),
                           modelName = std::move(modelName)]()
        {
        // B4: predict begin — also record executor queue wait (B4 - B3)
        int64_t maxExecQueueWait = 0;
        for (auto& t : *tasks)
        {
            if (t.perfTrace)
            {
                t.perfTrace->b4_predict_begin = t.perfTrace->nowUs();
                int64_t w = t.perfTrace->b4_predict_begin - t.perfTrace->b3_group_dispatch;
                if (w > maxExecQueueWait) maxExecQueueWait = w;
            }
        }
        MetricsCollector::instance().recordBatchExecutorQueueWait(modelName, maxExecQueueWait);

        auto engine = factory_->getModel(modelName);
        if (!engine)
        {
            std::string err = R"({"status":"error","message":"unknown model: )"
                            + modelName + "\"}";
            for (auto& t : *tasks)
            {
                if (t.slot) t.slot->resultJson = err;
                t.promise->set_value(err);
            }
            return;
        }

        std::vector<std::string> results;
        try
        {
            results = engine->predictBatch(*images);
        }
        catch (const std::exception& e)
        {
            LOG_ERROR << "DynamicBatchScheduler: predictBatch error for "
                      << modelName << ": " << e.what();
            std::string err = R"({"status":"error","message":")"
                            + std::string(e.what()) + "\"}";
            results.assign(tasks->size(), err);
        }

        // B5: predict done
        for (auto& t : *tasks)
            if (t.perfTrace)
                t.perfTrace->b5_predict_done = t.perfTrace->nowUs();

        // Write results + set promises.
        // Guard against predictBatch returning fewer results than tasks.
        for (size_t i = 0; i < tasks->size(); ++i)
        {
            if (i < results.size())
            {
                if ((*tasks)[i].slot)
                    (*tasks)[i].slot->resultJson = std::move(results[i]);
                (*tasks)[i].promise->set_value("ok");
            }
            else
            {
                // Safety net: predictBatch returned too few results.
                // Set an error so the client doesn't hang forever.
                std::string err = R"({"status":"error","message":"predictBatch: missing result for index )"
                                + std::to_string(i) + "\"}";
                LOG_ERROR << "DynamicBatchScheduler: missing result index=" << i
                          << " model=" << modelName;
                if ((*tasks)[i].slot)
                    (*tasks)[i].slot->resultJson = err;
                (*tasks)[i].promise->set_value(err);
            }

            // B6: promise set
            if ((*tasks)[i].perfTrace)
                (*tasks)[i].perfTrace->b6_promise_set = (*tasks)[i].perfTrace->nowUs();
        }
        }));
    }
    catch (const std::exception& e)
    {
        // executor submit() itself threw — tasks were never enqueued.
        // Use the backup pointer to fail all promises.
        LOG_ERROR << "DynamicBatchScheduler: executor submit failed for "
                  << modelName << ": " << e.what();
        std::string err = R"({"status":"error","message":"executor: )"
                        + std::string(e.what()) + "\"}";
        for (auto& t : *tasksBackup)
        {
            if (t.slot) t.slot->resultJson = err;
            t.promise->set_value(err);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// schedulerLoop — main scheduling loop
//
// Three-phase design:
//   Phase 1: if any queue is "hot" (>= preferred or max_batch), dispatch immediately.
//   Phase 2: check for timed-out requests, dispatch them.
//   Phase 3: if nothing dispatched, wait until nearest deadline or new work.
//
// The "first request opens a waiting window" semantics (Triton-like):
//   - When queue.size < min(preferred), we DO NOT dispatch.
//   - We wait until oldest.enqueueTime + maxQueueDelayUs.
//   - During the window, new items can arrive and push the queue over a
//     preferred threshold, triggering immediate dispatch.
//   - This prevents the degenerate bs=1-every-time behavior that
//     snapshot-based scheduling would produce.
// ─────────────────────────────────────────────────────────────────────────────

void DynamicBatchScheduler::schedulerLoop()
{
    using Clock = std::chrono::steady_clock;

    while (running_.load())
    {
        // ── Prune completed futures ──
        pendingFutures_.erase(
            std::remove_if(pendingFutures_.begin(), pendingFutures_.end(),
                [](std::future<void>& f) {
                    return f.valid()
                        && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                }),
            pendingFutures_.end());

        // ── Collect all non-empty queues and decide what to dispatch ──
        std::vector<BatchPlan> readyPlans;
        Clock::time_point nearestDeadline = Clock::time_point::max();
        bool anyWork = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto now = Clock::now();

            for (auto& [key, mq] : queues_)
            {
                if (mq.items.empty())
                    continue;

                anyWork = true;
                int n = static_cast<int>(mq.items.size());
                int64_t oldestWaitUs = duration_cast<microseconds>(
                    now - mq.items.front().enqueueTime).count();

                // Check if we should dispatch now:
                bool shouldDispatch = (n >= mq.maxBatchSize);
                if (!shouldDispatch && !mq.preferredBatchSizes.empty())
                    shouldDispatch = (n >= mq.preferredBatchSizes.front());
                if (!shouldDispatch)
                    shouldDispatch = (oldestWaitUs >= mq.maxQueueDelayUs);

                if (shouldDispatch)
                {
                    auto plan = decideBatch(mq, key);
                    if (!plan.items.empty())
                        readyPlans.push_back(std::move(plan));
                }
                else
                {
                    // In waiting window — track nearest deadline
                    auto deadline = mq.items.front().enqueueTime
                                    + microseconds(mq.maxQueueDelayUs);
                    if (deadline < nearestDeadline)
                        nearestDeadline = deadline;
                }
            }
        }

        // Execute ready plans outside the lock
        for (auto& plan : readyPlans)
            executeBatch(std::move(plan));

        if (!readyPlans.empty())
            continue;  // dispatched something, re-scan immediately

        // ── Wait ──
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!running_.load())
                break;

            if (!anyWork)
            {
                // No items — sleep until submit() wakes us
                cv_.wait(lock, [this] {
                    for (auto& [k, q] : queues_)
                        if (!q.items.empty()) return true;
                    return !running_.load();
                });
            }
            else if (nearestDeadline != Clock::time_point::max())
            {
                // Wait until the closest deadline OR new items trigger dispatch
                cv_.wait_until(lock, nearestDeadline, [this] {
                    auto now = Clock::now();
                    for (auto& [k, q] : queues_)
                    {
                        if (q.items.empty()) continue;
                        int n = static_cast<int>(q.items.size());
                        if (n >= q.maxBatchSize) return true;
                        if (!q.preferredBatchSizes.empty()
                            && n >= q.preferredBatchSizes.front()) return true;
                        if (duration_cast<microseconds>(
                                now - q.items.front().enqueueTime).count()
                            >= q.maxQueueDelayUs) return true;
                    }
                    return !running_.load();
                });
            }
            else
            {
                // Guard: shouldn't reach here
                cv_.wait_for(lock, milliseconds(100),
                             [this] { return !running_.load(); });
            }
        }
    }

    LOG_INFO << "DynamicBatchScheduler worker exited";
}

// ─────────────────────────────────────────────────────────────────────────────
// snapshot
// ─────────────────────────────────────────────────────────────────────────────

DynamicBatchScheduler::Snapshot DynamicBatchScheduler::snapshot() const
{
    Snapshot s;
    s.totalQueued = totalQueued_.load(std::memory_order_relaxed);
    s.dropped     = dropped_.load(std::memory_order_relaxed);
    s.batches     = batches_.load(std::memory_order_relaxed);
    s.requests    = requests_.load(std::memory_order_relaxed);
    return s;
}
