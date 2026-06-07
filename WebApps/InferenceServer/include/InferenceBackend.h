#pragma once

#include "ModelConfig.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace inference {

/// Structured container for one or more output tensors from a backend.
/// Detection models often produce multiple tensors (boxes, scores, classes);
/// classification models produce one (logits).
struct InferenceOutput {
    /// Flattened data from ALL output tensors concatenated together.
    /// May be empty when backendHandle is set (zero-copy path).
    std::vector<float> data;

    /// Shape of the concatenated output (batch dim first, e.g. {1, N}).
    std::vector<int64_t> shape;

    /// Per-output-tensor shapes (empty for single-output models).
    std::vector<std::vector<int64_t>> shapes;

    /// Per-output-tensor data slices (each is a view/copy of its portion of .data).
    std::vector<std::vector<float>> tensors;

    /// Output tensor names from the backend.
    std::vector<std::string> names;

    /// --- Zero-copy support ---
    /// Opaque handle that keeps backend-owned output memory alive.
    /// When set, data may be empty — use dataPtr() / totalElements() instead.
    std::shared_ptr<void> backendHandle;

    /// Pointer into backend-owned output memory (valid while backendHandle lives).
    /// Only set on the zero-copy path.
    const float* dataPtr = nullptr;
    size_t dataSize = 0;

    /// True when multi-output was used (vs. single tensor).
    bool isMultiOutput() const { return !tensors.empty(); }

    /// Data pointer: backend-owned if zero-copy, otherwise .data.data().
    const float* dataPtrOrCopy() const { return dataPtr ? dataPtr : data.data(); }

    /// Convenience: total element count including batch dim.
    size_t totalElements() const { return dataPtr ? dataSize : data.size(); }
};

/// Pure tensor-in, tensor-out backend.  No image knowledge, no labels.
class InferenceBackend
{
public:
    virtual ~InferenceBackend() = default;

    /// Single-sample inference (single output).
    /// @param input       flat float array (already preprocessed, CHW layout)
    /// @param inputShape  shape including batch dim, e.g. {1, 3, 224, 224}
    /// @param outputShape [out] shape of the output tensor (including batch dim)
    /// @return flat float array of output values
    virtual std::vector<float> infer(const std::vector<float>& input,
                                     const std::vector<int64_t>& inputShape,
                                     std::vector<int64_t>& outputShape) = 0;

    /// Single-sample inference with multi-output support.
    /// Default implementation delegates to infer() and wraps in InferenceOutput.
    virtual InferenceOutput inferMulti(const std::vector<float>& input,
                                       const std::vector<int64_t>& inputShape);

    /// Batch inference.  Default loops over infer() sequentially.
    virtual std::vector<float> inferBatch(const std::vector<float>& batchInput,
                                          const std::vector<int64_t>& batchShape,
                                          std::vector<int64_t>& outputShape);

    /// Batch inference with multi-output support.
    /// Default delegates to inferBatch() and wraps.
    virtual InferenceOutput inferBatchMulti(const std::vector<float>& batchInput,
                                            const std::vector<int64_t>& batchShape);

    virtual int  maxBatchSize() const { return 1; }
    virtual bool isReady() const { return true; }
};

} // namespace inference
