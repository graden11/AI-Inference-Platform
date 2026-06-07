#include "../include/ClassificationPostprocessor.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace inference {

ClassificationPostprocessor::ClassificationPostprocessor(int topK)
    : topK_(topK)
{
}

nlohmann::json ClassificationPostprocessor::postprocess(
    const InferenceOutput& output,
    const std::vector<std::string>& labels)
{
    return postprocessSample(output.data.data(), output.totalElements(), labels);
}

nlohmann::json ClassificationPostprocessor::postprocessSample(
    const float* logits,
    size_t numClasses,
    const std::vector<std::string>& labels)
{

    // Build index vector and partial-sort by logit value
    std::vector<int> indices(numClasses);
    for (size_t i = 0; i < numClasses; ++i)
        indices[i] = static_cast<int>(i);

    int k = std::min(topK_, static_cast<int>(numClasses));
    std::partial_sort(indices.begin(), indices.begin() + k, indices.end(),
                      [&](int a, int b) { return logits[a] > logits[b]; });

    // Numerically stable softmax
    float maxLogit = *std::max_element(logits, logits + numClasses);
    float sumExp = 0.0f;
    for (size_t i = 0; i < numClasses; ++i)
        sumExp += std::exp(logits[i] - maxLogit);

    // Build predictions JSON array
    nlohmann::json predictions = nlohmann::json::array();
    std::ostringstream summary;
    summary << std::fixed << std::setprecision(1);

    for (int i = 0; i < k; ++i)
    {
        int id = indices[i];
        float conf = std::exp(logits[id] - maxLogit) / sumExp * 100.0f;
        const std::string& label = (id < static_cast<int>(labels.size())) ? labels[id] : "unknown";

        nlohmann::json pred;
        pred["id"] = id;
        pred["label"] = label;
        pred["confidence"] = conf;
        predictions.push_back(pred);

        if (i == 0)
            summary << "识别结果：" << label << "（" << conf << "%）";
        else if (i == 1)
            summary << "，其他可能：" << label << "（" << conf << "%）";
        else
            summary << "、" << label << "（" << conf << "%）";
    }

    nlohmann::json resp;
    resp["status"] = "ok";
    resp["task_type"] = "classification";
    resp["summary"] = summary.str();
    resp["predictions"] = predictions;
    return resp;
}

} // namespace inference
