#include "../include/SegmentationPostprocessor.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

namespace inference {

SegmentationPostprocessor::SegmentationPostprocessor(const ModelConfig& config)
    : outputArgmax_(config.num_segmentation_classes > 0),
      numClasses_(config.num_segmentation_classes),
      outputHWC_(config.output.layout == "hwc")
{
}

nlohmann::json SegmentationPostprocessor::postprocess(
    const InferenceOutput& output,
    const std::vector<std::string>& labels)
{
    // Expected output shape: {1, C, H, W}    if outputArgmax == true
    //                      or {1, H, W}       if already class-map (outputArgmax == false)

    const float* raw = output.dataPtrOrCopy();
    size_t total = output.totalElements();

    if (total == 0)
    {
        nlohmann::json resp;
        resp["status"] = "ok";
        resp["task_type"] = "segmentation";
        resp["summary"] = "分割完成（空输出）";
        resp["mask"]["width"]  = 0;
        resp["mask"]["height"] = 0;
        resp["mask"]["num_classes"] = 0;
        resp["mask"]["data"] = "";
        return resp;
    }

    int realClasses = numClasses_;
    int maskH = 1, maskW = 1;

    if (output.shape.size() >= 4)
    {
        if (outputHWC_) {
            // {1, H, W, C} (HWC layout)
            maskH = static_cast<int>(output.shape[1]);
            maskW = static_cast<int>(output.shape[2]);
            realClasses = static_cast<int>(output.shape[3]);
        } else {
            // {1, C, H, W} (CHW layout)
            realClasses = static_cast<int>(output.shape[1]);
            maskH = static_cast<int>(output.shape[2]);
            maskW = static_cast<int>(output.shape[3]);
        }
        outputArgmax_ = true;
    }
    else if (output.shape.size() >= 3)
    {
        // {1, H, W} — already class map
        maskH = static_cast<int>(output.shape[1]);
        maskW = static_cast<int>(output.shape[2]);
        outputArgmax_ = false;
        realClasses = 1;
    }
    else if (output.shape.size() >= 2)
    {
        // {1, H*W} — flat class map
        int totalPixels = static_cast<int>(output.shape[1]);
        maskH = totalPixels; // approximate; better to use config
        maskW = 1;
        outputArgmax_ = false;
    }

    // Build class map
    std::vector<uint8_t> classMap;
    classMap.reserve(maskH * maskW);

    if (outputArgmax_)
    {
        // Argmax over C dimension
        int totalPixels = maskH * maskW;
        for (int p = 0; p < totalPixels; ++p)
        {
            const float* pixel = raw + p; // stride by C interleaved? Actually C is outer
        }
        // CHW layout: for each spatial position, find max across channels
        for (int y = 0; y < maskH; ++y)
        {
            for (int x = 0; x < maskW; ++x)
            {
                float maxVal = -INFINITY;
                uint8_t bestClass = 0;
                for (int c = 0; c < realClasses; ++c)
                {
                    int idx = outputHWC_ ? (y * maskW + x) * realClasses + c : c * maskH * maskW + y * maskW + x;
                    float val = raw[idx];
                    if (val > maxVal) { maxVal = val; bestClass = static_cast<uint8_t>(c); }
                }
                classMap.push_back(bestClass);
            }
        }
    }
    else
    {
        // Already a class map — cast to uint8
        classMap.assign(raw, raw + total);
    }

    // Encode class map as base64 (raw bytes — compact for INT masks)
    if (numClasses_ <= 0) numClasses_ = realClasses;

    nlohmann::json maskObj;
    maskObj["width"]  = maskW;
    maskObj["height"] = maskH;
    maskObj["num_classes"] = numClasses_;

    // Simple base64 encode of raw class-map bytes
    static const char kBase64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    b64.reserve(((classMap.size() + 2) / 3) * 4);
    for (size_t i = 0; i < classMap.size(); i += 3)
    {
        uint32_t val = static_cast<uint32_t>(classMap[i]) << 16;
        if (i + 1 < classMap.size()) val |= static_cast<uint32_t>(classMap[i+1]) << 8;
        if (i + 2 < classMap.size()) val |= static_cast<uint32_t>(classMap[i+2]);
        b64 += kBase64[(val >> 18) & 63];
        b64 += kBase64[(val >> 12) & 63];
        b64 += kBase64[(val >>  6) & 63];
        b64 += kBase64[ val        & 63];
    }
    // Adjust padding
    size_t pad = (3 - classMap.size() % 3) % 3;
    for (size_t p = 0; p < pad; ++p)
        b64[b64.size() - 1 - p] = '=';

    maskObj["data"] = b64;

    std::ostringstream summary;
    summary << "分割完成" << "，尺寸 " << maskW << "×" << maskH
            << "，" << numClasses_ << " 个类别";

    nlohmann::json resp;
    resp["status"]     = "ok";
    resp["task_type"]  = "segmentation";
    resp["summary"]    = summary.str();
    resp["mask"]       = maskObj;
    return resp;
}

std::vector<nlohmann::json> SegmentationPostprocessor::postprocessBatch(
    const InferenceOutput& batchOutput,
    int batchSize,
    const std::vector<std::string>& labels)
{
    // Segmentation batch: split by batch dim
    std::vector<nlohmann::json> results;
    results.reserve(batchSize);

    size_t perSampleElems = batchOutput.totalElements() / batchSize;

    for (int i = 0; i < batchSize; ++i)
    {
        auto begin = batchOutput.data.begin() + i * perSampleElems;
        auto end   = begin + perSampleElems;

        InferenceOutput singleIO;
        singleIO.data.assign(begin, end);
        // Shift batch dim: {N, ...} → {1, ...}
        singleIO.shape = batchOutput.shape;
        if (!singleIO.shape.empty())
            singleIO.shape[0] = 1;

        results.push_back(postprocess(singleIO, labels));
    }

    return results;
}

} // namespace inference
