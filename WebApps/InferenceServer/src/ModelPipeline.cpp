#include "../include/ModelPipeline.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <muduo/base/Logging.h>

#include "../../../HttpServer/include/utils/JsonUtil.h"
#include "../../../HttpServer/include/utils/MetricsCollector.h"

namespace inference {

// ---------------------------------------------------------------------------
// Helper: load labels file (same logic as old InferenceEngine::loadLabels)
// ---------------------------------------------------------------------------
static std::vector<std::string> loadLabels(const std::string& path)
{
    std::vector<std::string> labels;
    if (path.empty()) return labels;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line))
    {
        if (!line.empty())
            labels.push_back(line);
    }
    return labels;
}

// ---------------------------------------------------------------------------
// Helper: read entire file into bytes
// ---------------------------------------------------------------------------
static std::vector<uint8_t> readFileBytes(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// ---------------------------------------------------------------------------
// ModelPipeline
// ---------------------------------------------------------------------------
ModelPipeline::ModelPipeline(ModelConfig config,
                             std::unique_ptr<Preprocessor> preprocessor,
                             std::unique_ptr<InferenceBackend> backend,
                             std::unique_ptr<Postprocessor> postprocessor)
    : config_(std::move(config)),
      preprocessor_(std::move(preprocessor)),
      backend_(std::move(backend)),
      postprocessor_(std::move(postprocessor)),
      labels_(loadLabels(config_.labels_path))
{
    LOG_INFO << "ModelPipeline created: " << config_.name << ":" << config_.version
             << " type=" << config_.type
             << " task=" << taskTypeToString(config_.task)
             << " labels=" << labels_.size();
}

int ModelPipeline::maxBatchSize() const
{
    return backend_->maxBatchSize();
}

nlohmann::json ModelPipeline::doPredictJson(const std::vector<uint8_t>& imageBytes)
{
    LOG_INFO << "ModelPipeline::doPredictJson task=" << taskTypeToString(config_.task)
             << " model=" << config_.name << " bytes=" << imageBytes.size();

    // 1. Preprocess: image bytes → CHW float tensor
    auto input = preprocessor_->preprocess(imageBytes);
    if (input.empty())
    {
        nlohmann::json err;
        err["status"] = "error";
        err["message"] = "failed to decode/preprocess image";
        return err;
    }
    LOG_INFO << "doPredictJson: preprocess done, tensor size=" << input.size();

    // 2. Build input shape (batch=1 + spatial dims from config)
    bool isHWC = config_.input.layout == "hwc";
    std::vector<int64_t> inputShape = {
        1,
        isHWC ? config_.input.preferred_height : config_.input.channels,
        isHWC ? config_.input.preferred_width  : config_.input.preferred_height,
        isHWC ? config_.input.channels         : config_.input.preferred_width
    };

    // 3. Infer: use inferMulti for future multi-output support
    LOG_INFO << "doPredictJson: calling infer, inputShape=["
             << inputShape[0] << "," << inputShape[1] << ","
             << inputShape[2] << "," << inputShape[3] << "]";
    InferenceOutput inferOut;
    try {
        auto t0 = std::chrono::steady_clock::now();
        inferOut = backend_->inferMulti(input, inputShape);
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count();
        MetricsCollector::instance().recordModelLatency(config_.name,
            taskTypeToString(config_.task), elapsed, 1);
    } catch (const std::exception& e) {
        LOG_ERROR << "doPredictJson: infer threw exception: " << e.what();
        nlohmann::json err;
        err["status"] = "error";
        err["message"] = std::string("inference error: ") + e.what();
        return err;
    } catch (...) {
        LOG_ERROR << "doPredictJson: infer threw unknown exception";
        nlohmann::json err;
        err["status"] = "error";
        err["message"] = "inference error: unknown exception";
        return err;
    }
    LOG_INFO << "doPredictJson: infer done, output size=" << inferOut.totalElements()
             << " shape[0]=" << (inferOut.shape.empty() ? -1 : inferOut.shape[0]);

    // 4. Postprocess: tensor → JSON
    LOG_INFO << "doPredictJson: calling postprocess";
    auto result = postprocessor_->postprocess(inferOut, labels_);
    LOG_INFO << "doPredictJson: done, status=" << result.value("status", "?");
    return result;
}

nlohmann::json ModelPipeline::predictFromBytesJson(const std::vector<uint8_t>& imageData)
{
    return doPredictJson(imageData);
}

std::string ModelPipeline::predictFromBytes(const std::vector<uint8_t>& imageData)
{
    return doPredictJson(imageData).dump();
}

std::string ModelPipeline::predict(const std::string& imagePath)
{
    auto fileBytes = readFileBytes(imagePath);
    if (fileBytes.empty())
    {
        nlohmann::json err;
        err["status"] = "error";
        err["message"] = std::string("failed to load image: ") + imagePath;
        return err.dump();
    }
    return predictFromBytes(fileBytes);
}

std::vector<std::string> ModelPipeline::predictBatch(
    const std::vector<std::vector<uint8_t>>& images)
{
    int batchSize = static_cast<int>(images.size());
    if (batchSize == 0)
        return {};

    // 1. Preprocess all images, concatenate into one NCHW tensor
    int perSampleElems = config_.input.elemCount();
    thread_local std::vector<float> batchInput;
    batchInput.clear();
    batchInput.reserve(batchSize * perSampleElems);

    for (auto& img : images)
    {
        auto input = preprocessor_->preprocess(img);
        if (input.empty())
        {
            LOG_ERROR << "predictBatch: failed to decode image, zero-filling";
            batchInput.insert(batchInput.end(), perSampleElems, 0.0f);
        }
        else
        {
            batchInput.insert(batchInput.end(), input.begin(), input.end());
        }
    }

    // 2. Batch infer
    bool isHWC = config_.input.layout == "hwc";    std::vector<int64_t> batchShape = {
        batchSize,
        isHWC ? config_.input.preferred_height : config_.input.channels,
        isHWC ? config_.input.preferred_width  : config_.input.preferred_height,
        isHWC ? config_.input.channels         : config_.input.preferred_width
    };
    auto batchIO = backend_->inferBatchMulti(batchInput, batchShape);

    // 3. Postprocess — delegate to per-task postprocessBatch
    auto resultsJson = postprocessor_->postprocessBatch(batchIO, batchSize, labels_);

    std::vector<std::string> results;
    results.reserve(resultsJson.size());
    for (auto& j : resultsJson)
        results.push_back(j.dump());
    return results;
}

} // namespace inference
