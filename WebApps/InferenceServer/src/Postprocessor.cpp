#include "../include/Postprocessor.h"
#include "../include/ClassificationPostprocessor.h"
#include "../include/DetectionPostprocessor.h"
#include "../include/SegmentationPostprocessor.h"
#include "../include/FeatureExtractionPostprocessor.h"
#include "../include/ModelConfig.h"

namespace inference {

nlohmann::json Postprocessor::postprocess(const std::vector<float>& output,
                                           const std::vector<int64_t>& outputShape,
                                           const std::vector<std::string>& labels)
{
    InferenceOutput io;
    io.data  = output;
    io.shape = outputShape;
    return postprocess(io, labels);
}

std::vector<nlohmann::json> Postprocessor::postprocessBatch(
    const InferenceOutput& batchOutput,
    int batchSize,
    const std::vector<std::string>& labels)
{
    // Default: split evenly by outputShape[1] (works for classification).
    // Uses postprocessSample() — a zero-copy view — instead of building a
    // temporary std::vector<float> copy per sample.
    std::vector<nlohmann::json> results;
    results.reserve(batchSize);

    size_t perSampleOut = batchOutput.shape.size() >= 2
        ? static_cast<size_t>(batchOutput.shape[1])
        : batchOutput.data.size() / batchSize;

    for (int i = 0; i < batchSize; ++i)
    {
        const float* sample = batchOutput.data.data() + i * perSampleOut;
        results.push_back(postprocessSample(sample, perSampleOut, labels));
    }
    return results;
}

std::unique_ptr<Postprocessor> createPostprocessor(const ModelConfig& config)
{
    switch (config.task)
    {
        case TaskType::CLASSIFICATION:
            return std::make_unique<ClassificationPostprocessor>(config.top_k);
        case TaskType::DETECTION:
            return std::make_unique<DetectionPostprocessor>(config);
        case TaskType::SEGMENTATION:
            return std::make_unique<SegmentationPostprocessor>(config);
        case TaskType::FEATURE_EXTRACTION:
            return std::make_unique<FeatureExtractionPostprocessor>();
    }
    // fallback
    return std::make_unique<ClassificationPostprocessor>(config.top_k);
}

} // namespace inference
