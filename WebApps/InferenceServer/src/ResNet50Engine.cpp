#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE2_IMPLEMENTATION
#include <stb_image_resize2.h>

#include "../include/ResNet50Engine.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <muduo/base/Logging.h>

#include "../../../HttpServer/include/utils/JsonUtil.h"

ResNet50Engine::ResNet50Engine(const std::string &modelPath, const std::string &labelsPath,
                               int maxBatchSize)
    : env_(ORT_LOGGING_LEVEL_WARNING, "resnet"),
      memInfo_("Cpu", OrtArenaAllocator, 0, OrtMemTypeDefault),
      maxBatchSize_(maxBatchSize)
{
    sessionOpts_.SetIntraOpNumThreads(1);
    session_ = std::make_unique<Ort::Session>(env_, modelPath.c_str(), sessionOpts_);
    labels_ = loadLabels(labelsPath);
    LOG_INFO << "ResNet50Engine initialized, model: " << modelPath
             << ", labels: " << labels_.size()
             << ", maxBatchSize: " << maxBatchSize_;
}

ResNet50Engine::~ResNet50Engine() = default;

std::vector<float> ResNet50Engine::preprocess(const uint8_t *rgbData, int w, int h, int channels)
{
    int targetW = INPUT_W;
    int targetH = INPUT_H;
    int targetC = INPUT_C;

    // Resize to 224x224 using bilinear interpolation
    thread_local std::vector<uint8_t> resized(targetH * targetW * targetC);
    stbir_resize_uint8_srgb(rgbData, w, h, 0,
                            resized.data(), targetW, targetH, 0,
                            static_cast<stbir_pixel_layout>(targetC));

    // Normalize, standardize, HWC→CHW
    int elemCount = targetC * targetH * targetW;
    thread_local std::vector<float> input(elemCount);

    for (int c = 0; c < targetC; ++c)
    {
        for (int y = 0; y < targetH; ++y)
        {
            for (int x = 0; x < targetW; ++x)
            {
                int hwcIdx = (y * targetW + x) * targetC + c;
                int chwIdx = (c * targetH + y) * targetW + x;
                float val = resized[hwcIdx] / 255.0f;
                input[chwIdx] = (val - MEAN[c]) / STD[c];
            }
        }
    }

    return input;
}

std::vector<std::pair<int, float>> ResNet50Engine::runInference(const std::vector<float> &inputTensor)
{
    std::vector<int64_t> shape = {1, INPUT_C, INPUT_H, INPUT_W};

    Ort::Value inputValue = Ort::Value::CreateTensor<float>(
        memInfo_,
        const_cast<float *>(inputTensor.data()),
        inputTensor.size(),
        shape.data(),
        shape.size());

    const char *inputNames[] = {INPUT_NAME};
    const char *outputNames[] = {OUTPUT_NAME};

    std::lock_guard<std::mutex> lock(inferenceMutex_);
    auto outputValues = session_->Run(Ort::RunOptions{},
                                      inputNames, &inputValue, 1,
                                      outputNames, 1);

    float *logits = outputValues[0].GetTensorMutableData<float>();
    auto typeInfo = outputValues[0].GetTensorTypeAndShapeInfo();
    size_t numClasses = typeInfo.GetElementCount();

    return softmaxTopK(logits, numClasses);
}

std::string ResNet50Engine::predict(const std::string &imagePath)
{
    int w, h, channels;
    unsigned char *data = stbi_load(imagePath.c_str(), &w, &h, &channels, 3);
    if (!data)
    {
        LOG_ERROR << "Failed to load image: " << imagePath;
        json err;
        err["status"] = "error";
        err["message"] = std::string("failed to load image: ") + imagePath;
        return err.dump();
    }

    auto input = preprocess(data, w, h, 3);
    stbi_image_free(data);

    auto results = runInference(input);
    return buildPredictionsJson(results, labels_).dump();
}

std::string ResNet50Engine::predictFromBytes(const std::vector<uint8_t> &imageData)
{
    int w, h, channels;
    unsigned char *data = stbi_load_from_memory(
        imageData.data(), static_cast<int>(imageData.size()),
        &w, &h, &channels, 3);
    if (!data)
    {
        LOG_ERROR << "Failed to decode image from memory";
        json err;
        err["status"] = "error";
        err["message"] = "failed to decode image from memory";
        return err.dump();
    }

    auto input = preprocess(data, w, h, 3);
    stbi_image_free(data);

    auto results = runInference(input);
    return buildPredictionsJson(results, labels_).dump();
}

std::vector<std::string> ResNet50Engine::predictBatch(
    const std::vector<std::vector<uint8_t>> &images)
{
    int batchSize = static_cast<int>(images.size());
    if (batchSize == 0)
        return {};

    // Decode and preprocess all images, concatenate into one NCHW tensor
    const int elemPerImage = INPUT_C * INPUT_H * INPUT_W;
    std::vector<float> batchInput;
    batchInput.reserve(batchSize * elemPerImage);

    for (auto &img : images)
    {
        int w, h, channels;
        unsigned char *data = stbi_load_from_memory(
            img.data(), static_cast<int>(img.size()), &w, &h, &channels, 3);
        if (!data)
        {
            LOG_ERROR << "predictBatch: failed to decode image, skipping";
            batchInput.insert(batchInput.end(), elemPerImage, 0.0f);
            continue;
        }
        auto input = preprocess(data, w, h, 3);
        stbi_image_free(data);
        batchInput.insert(batchInput.end(), input.begin(), input.end());
    }

    // Single inference with batch shape
    std::vector<int64_t> shape = {batchSize, INPUT_C, INPUT_H, INPUT_W};

    Ort::Value inputValue = Ort::Value::CreateTensor<float>(
        memInfo_, batchInput.data(), batchInput.size(), shape.data(), shape.size());

    const char *inputNames[] = {INPUT_NAME};
    const char *outputNames[] = {OUTPUT_NAME};

    std::lock_guard<std::mutex> lock(inferenceMutex_);
    auto outputValues = session_->Run(Ort::RunOptions{},
                                      inputNames, &inputValue, 1,
                                      outputNames, 1);

    float *logits = outputValues[0].GetTensorMutableData<float>();
    auto typeInfo = outputValues[0].GetTensorTypeAndShapeInfo();
    size_t totalElements = typeInfo.GetElementCount();
    size_t numClasses = totalElements / batchSize;

    std::vector<std::string> results;
    results.reserve(batchSize);
    for (int i = 0; i < batchSize; ++i)
    {
        auto topk = softmaxTopK(logits + i * numClasses, numClasses);
        results.push_back(buildPredictionsJson(topk, labels_).dump());
    }
    return results;
}
