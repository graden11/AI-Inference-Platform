#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../../../HttpServer/include/utils/JsonUtil.h"

class InferenceEngine
{
public:
    virtual ~InferenceEngine() = default;

    virtual std::string predict(const std::string &imagePath) = 0;
    virtual std::string predictFromBytes(const std::vector<uint8_t> &imageData) = 0;

    virtual int maxBatchSize() const { return 1; }

    virtual std::vector<std::string> predictBatch(const std::vector<std::vector<uint8_t>> &images)
    {
        std::vector<std::string> results;
        results.reserve(images.size());
        for (auto &img : images)
            results.push_back(predictFromBytes(img));
        return results;
    }

    static constexpr const char *INPUT_NAME = "input";
    static constexpr const char *OUTPUT_NAME = "output";
    static constexpr int INPUT_W = 224;
    static constexpr int INPUT_H = 224;
    static constexpr int INPUT_C = 3;
    static constexpr float MEAN[3] = {0.485f, 0.456f, 0.406f};
    static constexpr float STD[3] = {0.229f, 0.224f, 0.225f};

protected:
    static std::vector<std::string> loadLabels(const std::string &path)
    {
        std::vector<std::string> labels;
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line))
        {
            if (!line.empty())
                labels.push_back(line);
        }
        return labels;
    }

    static std::vector<std::pair<int, float>> softmaxTopK(const float *logits, size_t numClasses, int topK = 5)
    {
        std::vector<int> indices(numClasses);
        for (size_t i = 0; i < numClasses; ++i)
            indices[i] = static_cast<int>(i);

        topK = std::min(topK, static_cast<int>(numClasses));
        std::partial_sort(indices.begin(), indices.begin() + topK, indices.end(),
                          [&](int a, int b) { return logits[a] > logits[b]; });

        float sumExp = 0.0f;
        for (size_t i = 0; i < numClasses; ++i)
            sumExp += std::exp(logits[i]);

        std::vector<std::pair<int, float>> results;
        results.reserve(topK);
        for (int i = 0; i < topK; ++i)
        {
            int id = indices[i];
            results.emplace_back(id, std::exp(logits[id]) / sumExp * 100.0f);
        }
        return results;
    }

    static json buildPredictionsJson(const std::vector<std::pair<int, float>> &results,
                                     const std::vector<std::string> &labels)
    {
        json predictions = json::array();
        std::ostringstream summary;
        summary << std::fixed << std::setprecision(1);

        for (size_t i = 0; i < results.size(); ++i)
        {
            auto &[id, conf] = results[i];
            const std::string &label = (id < static_cast<int>(labels.size())) ? labels[id] : "unknown";

            json pred;
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

        json resp;
        resp["status"] = "ok";
        resp["summary"] = summary.str();
        resp["predictions"] = predictions;
        return resp;
    }
};
