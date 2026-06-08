#pragma once

#include "InferenceBackend.h"

#include <cstdint>
#include <string>
#include <vector>

namespace inference {

/// Sends preprocessed tensors to a remote GPU server over HTTP.
/// Used when the cloud deployment routes GPU model requests through
/// an frp tunnel to a local machine with NVIDIA hardware.
///
/// Protocol: POST /predict/tensor with JSON body:
///   { "input": [...], "input_shape": [...], "batch_size": N }
/// Response: InferenceOutput serialized as JSON.
class RemoteBackend : public InferenceBackend
{
public:
    explicit RemoteBackend(const ModelConfig& config);
    ~RemoteBackend() override = default;

    std::vector<float> infer(const std::vector<float>& input,
                             const std::vector<int64_t>& inputShape,
                             std::vector<int64_t>& outputShape) override;

    InferenceOutput inferMulti(const std::vector<float>& input,
                               const std::vector<int64_t>& inputShape) override;

    std::vector<float> inferBatch(const std::vector<float>& batchInput,
                                  const std::vector<int64_t>& batchShape,
                                  std::vector<int64_t>& outputShape) override;

    InferenceOutput inferBatchMulti(const std::vector<float>& batchInput,
                                    const std::vector<int64_t>& batchShape) override;

    int  maxBatchSize() const override { return maxBatchSize_; }
    bool isReady()    const override;

private:
    std::string remoteUrl_;
    int maxBatchSize_;
    int timeoutMs_;

    /// Serialize inference request to JSON, POST to remote GPU, parse InferenceOutput.
    InferenceOutput remoteCall(const std::vector<float>& input,
                               const std::vector<int64_t>& inputShape,
                               int batchSize);
};

} // namespace inference
