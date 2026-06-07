#pragma once

#include "ModelConfig.h"
#include "InferenceBackend.h"  // for InferenceOutput

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace inference {

/// Converts raw output tensors into a structured JSON result.
class Postprocessor
{
public:
    virtual ~Postprocessor() = default;

    /// Single-sample postprocess using InferenceOutput (supports multi-output models).
    /// @param output   structured output container from the backend
    /// @param labels   class label strings (may be empty for some tasks)
    /// @return structured JSON (not yet serialized to string)
    virtual nlohmann::json postprocess(const InferenceOutput& output,
                                       const std::vector<std::string>& labels) = 0;

    /// Convenience overload: single flat tensor (for classification compatibility).
    /// Delegates to postprocess(InferenceOutput, labels) after wrapping.
    nlohmann::json postprocess(const std::vector<float>& output,
                               const std::vector<int64_t>& outputShape,
                               const std::vector<std::string>& labels);

    /// Single-sample postprocess using a slice of the batch output (no copy).
    /// @param data    pointer into the batch output buffer for this sample
    /// @param count   element count for this single sample
    /// @param labels  class label strings (may be empty for some tasks)
    /// @return structured JSON (not yet serialized to string)
    /// Default builds a temporary InferenceOutput and calls postprocess(io, labels).
    /// Subclasses that only read from data/shape may override to avoid the copy.
    virtual nlohmann::json postprocessSample(const float* data,
                                             size_t count,
                                             const std::vector<std::string>& labels)
    {
        InferenceOutput io;
        io.data.assign(data, data + count);
        io.shape = {1, static_cast<int64_t>(count)};
        return postprocess(io, labels);
    }

    /// Batch postprocess. Default splits by outputShape[1] and calls
    /// postprocessSample() per sample (zero-copy) instead of building a
    /// temporary vector per sample. Detection/segmentation must override if
    /// outputs are not evenly divisible per sample.
    virtual std::vector<nlohmann::json> postprocessBatch(
        const InferenceOutput& batchOutput,
        int batchSize,
        const std::vector<std::string>& labels);
};

/// Factory: creates the appropriate Postprocessor for a given ModelConfig.
std::unique_ptr<Postprocessor> createPostprocessor(const ModelConfig& config);

} // namespace inference
