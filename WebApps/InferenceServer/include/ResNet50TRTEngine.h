#pragma once

#include "InferenceEngine.h"

#include <NvInfer.h>
#include <cuda_runtime.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class ResNet50TRTEngine : public InferenceEngine
{
public:
    ResNet50TRTEngine(const std::string &enginePath, const std::string &labelsPath,
                      int maxBatchSize = 1);
    ~ResNet50TRTEngine() override;

    std::string predict(const std::string &imagePath) override;
    std::string predictFromBytes(const std::vector<uint8_t> &imageData) override;

    int maxBatchSize() const override { return maxBatchSize_; }
    std::vector<std::string> predictBatch(const std::vector<std::vector<uint8_t>> &images) override;

private:
    struct BufferSlot {
        uint8_t *h_resized_pinned = nullptr;
        float   *h_input_pinned   = nullptr;
        void    *d_input           = nullptr;
        void    *d_output          = nullptr;
        float   *h_output_pinned   = nullptr;
        bool     in_use            = false;
    };

    static constexpr int kNumSlots = 2;

    int  acquireSlot();
    void releaseSlot(int idx);

    void preprocess(const uint8_t *rgbData, int w, int h, int channels,
                    uint8_t *h_resized_pinned, float *h_input_pinned);
    void runInference(int slotIdx, std::vector<std::pair<int, float>> &results);
    bool loadEngine(const std::string &enginePath);
    static bool buildEngine(const std::string &onnxPath, const std::string &enginePath);

    class Logger : public nvinfer1::ILogger
    {
        void log(Severity severity, const char *msg) noexcept override;
    };

    Logger logger_;
    std::unique_ptr<nvinfer1::IRuntime> runtime_;
    std::unique_ptr<nvinfer1::ICudaEngine> engine_;
    std::unique_ptr<nvinfer1::IExecutionContext> context_;
    std::vector<std::string> labels_;

    size_t inputSize_{0};
    size_t outputSize_{0};
    int inputIndex_{-1};
    int outputIndex_{-1};

    cudaStream_t stream_{};
    std::mutex gpu_mutex_;

    BufferSlot slots_[kNumSlots];
    std::mutex slot_pool_mutex_;
    std::condition_variable slot_pool_cv_;

    // Batch buffers
    int maxBatchSize_{1};
    float *h_batch_input_{nullptr};
    float *h_batch_output_{nullptr};
    void *d_batch_input_{nullptr};
    void *d_batch_output_{nullptr};
    size_t perSampleInputSize_{0};
    size_t perSampleOutputSize_{0};
    void allocateBatchBuffers();
};
