#pragma once

#include "Postprocessor.h"

namespace inference {

/// Segmentation postprocessor: argmax + mask encoding (base64 raw) + JSON formatting.
class SegmentationPostprocessor : public Postprocessor
{
public:
    explicit SegmentationPostprocessor(const ModelConfig& config);

    nlohmann::json postprocess(const InferenceOutput& output,
                               const std::vector<std::string>& labels) override;

    std::vector<nlohmann::json> postprocessBatch(
        const InferenceOutput& batchOutput,
        int batchSize,
        const std::vector<std::string>& labels) override;

private:
    bool outputArgmax_;
    int  numClasses_;
    bool outputHWC_;  // true = output is HWC, false = CHW
};

} // namespace inference
