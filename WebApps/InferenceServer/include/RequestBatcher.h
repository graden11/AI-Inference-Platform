#pragma once

#include "ModelFactory.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RequestBatcher
{
public:
    struct Item
    {
        std::string modelName;
        std::vector<uint8_t> imageData;
        std::shared_ptr<std::promise<std::string>> promise;
        std::chrono::steady_clock::time_point enqueueTime;
    };

    RequestBatcher(ModelFactory* factory, int maxBatchSize,
                   std::chrono::milliseconds maxDelayMs);
    ~RequestBatcher();

    void start();
    void stop();

    std::future<std::string> submit(std::string modelName,
                                    std::vector<uint8_t> imageData);

private:
    void workerLoop();

    ModelFactory* factory_;
    int maxBatchSize_;
    std::chrono::milliseconds maxDelayMs_;

    std::deque<Item> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> running_{false};
};
