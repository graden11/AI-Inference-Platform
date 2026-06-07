#pragma once

#include "InferenceBackend.h"

#include <onnxruntime_cxx_api.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace inference {

/// ONNX Runtime backend — pure tensor-in, tensor-out.
class OnnxBackend : public InferenceBackend
{
public:
    explicit OnnxBackend(const ModelConfig& config);
    ~OnnxBackend() override = default;

    std::vector<float> infer(const std::vector<float>& input,
                             const std::vector<int64_t>& inputShape,
                             std::vector<int64_t>& outputShape) override;

    std::vector<float> inferBatch(const std::vector<float>& batchInput,
                                  const std::vector<int64_t>& batchShape,
                                  std::vector<int64_t>& outputShape) override;

    /// Zero-copy single-sample: holds Ort::Value via InferenceOutput.backendHandle.
    InferenceOutput inferMulti(const std::vector<float>& input,
                               const std::vector<int64_t>& inputShape) override;

    /// Zero-copy batch: holds Ort::Value via InferenceOutput.backendHandle.
    InferenceOutput inferBatchMulti(const std::vector<float>& batchInput,
                                    const std::vector<int64_t>& batchShape) override;

    int  maxBatchSize() const override { return maxBatchSize_; }
    int  detectedWidth()  const { return detectedWidth_; }
    int  detectedHeight() const { return detectedHeight_; }
    int  detectedChannels() const { return detectedChannels_; }
    const std::string& detectedLayout() const { return detectedLayout_; }
    const std::string& detectedTask() const { return detectedTask_; }
    bool detectedShape() const { return detectedShape_; }
    const std::string& inputName()  const { return inputName_; }
    const std::string& outputName() const { return outputName_; }
    bool isReady() const override { return session_ != nullptr; }

private:
    Ort::Env env_;
    Ort::SessionOptions sessionOpts_;
    std::unique_ptr<Ort::Session> session_;
    Ort::MemoryInfo memInfo_;

    std::string inputName_;
    std::string outputName_;
    int maxBatchSize_;
    int detectedWidth_ = 224;
    int detectedHeight_ = 224;
    int detectedChannels_ = 3;
    std::string detectedLayout_ = "chw";
    std::string detectedTask_;
    bool detectedShape_ = false;
    std::mutex inferenceMutex_;
};

} // namespace inference
