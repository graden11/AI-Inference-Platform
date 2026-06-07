#pragma once

#include "Postprocessor.h"

namespace inference {

/// Classification postprocessor: softmax + top-K + JSON formatting.
/// Produces the same JSON schema as the old InferenceEngine::buildPredictionsJson().
class ClassificationPostprocessor : public Postprocessor
{
public:
    explicit ClassificationPostprocessor(int topK = 5);

    nlohmann::json postprocess(const InferenceOutput& output,
                               const std::vector<std::string>& labels) override;

    /// Override to avoid the temporary InferenceOutput copy in postprocessSample.
    nlohmann::json postprocessSample(const float* data,
                                     size_t count,
                                     const std::vector<std::string>& labels) override;

private:
    int topK_;
};

} // namespace inference
