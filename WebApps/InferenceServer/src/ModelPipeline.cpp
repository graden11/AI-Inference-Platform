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
// Per-phase latency recording helpers
// ---------------------------------------------------------------------------
namespace {
    using Clock = std::chrono::steady_clock;
    using Us    = std::chrono::microseconds;

    struct PhaseTimer {
        Clock::time_point start;
        std::string modelName;
        std::string taskType;

        explicit PhaseTimer(std::string model, std::string task)
            : start(Clock::now()), modelName(std::move(model)), taskType(std::move(task)) {}

        int64_t record(const char* phase)
        {
            int64_t elapsed = std::chrono::duration_cast<Us>(
                Clock::now() - start).count();
            MetricsCollector::instance().recordPhaseLatency(
                modelName, taskType, phase, elapsed);
            start = Clock::now();  // reset for next phase
            return elapsed;
        }
    };
} // anonymous namespace

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

    std::string taskStr = taskTypeToString(config_.task);
    PhaseTimer timer(config_.name, taskStr);

    // 1. Preprocess: image bytes → CHW float tensor into thread-local buffer
    thread_local std::vector<float> t_input;
    if (!preprocessor_->preprocess(imageBytes, t_input))
    {
        nlohmann::json err;
        err["status"] = "error";
        err["message"] = "failed to decode/preprocess image";
        return err;
    }
    timer.record("preprocess");

    // 2. Build input shape (batch=1 + spatial dims from config)
    bool isHWC = config_.input.layout == "hwc";
    std::vector<int64_t> inputShape = {
        1,
        isHWC ? config_.input.preferred_height : config_.input.channels,
        isHWC ? config_.input.preferred_width  : config_.input.preferred_height,
        isHWC ? config_.input.channels         : config_.input.preferred_width
    };

    // 3. Infer: use inferMulti for future multi-output support
    InferenceOutput inferOut;
    try {
        inferOut = backend_->inferMulti(t_input, inputShape);
        int64_t inferElapsed = timer.record("inference");
        MetricsCollector::instance().recordModelLatency(config_.name, taskStr, inferElapsed, 1);
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

    // 4. Postprocess: tensor → JSON
    auto result = postprocessor_->postprocess(inferOut, labels_);
    timer.record("postprocess");

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

    int maxBatch = maxBatchSize();

    // Fallback for models with static batch=1 (e.g. squeezenet):
    // process each sample individually to avoid ONNX dimension errors.
    if (maxBatch <= 1)
    {
        std::vector<std::string> results;
        results.reserve(images.size());
        for (auto& img : images)
            results.push_back(predictFromBytes(img));
        return results;
    }

    int perSampleElems = config_.input.elemCount();
    thread_local std::vector<float> batchInput;

    // Process in chunks when batch exceeds the backend limit.
    std::vector<std::string> results;
    for (int start = 0; start < batchSize; start += maxBatch)
    {
        int chunk = std::min(maxBatch, batchSize - start);
        batchInput.assign(chunk * perSampleElems, 0.0f);

        for (int i = 0; i < chunk; ++i)
        {
            if (!preprocessor_->preprocessInto(images[start + i], batchInput,
                                               i * perSampleElems))
            {
                LOG_ERROR << "predictBatch: failed to decode image "
                          << (start + i) << ", zero-filling";
            }
        }

        bool isHWC = config_.input.layout == "hwc";
        std::vector<int64_t> chunkShape = {
            chunk,
            isHWC ? config_.input.preferred_height : config_.input.channels,
            isHWC ? config_.input.preferred_width  : config_.input.preferred_height,
            isHWC ? config_.input.channels         : config_.input.preferred_width
        };
        auto batchIO = backend_->inferBatchMulti(batchInput, chunkShape);
        auto resultsJson = postprocessor_->postprocessBatch(batchIO, chunk, labels_);

        for (auto& j : resultsJson)
            results.push_back(j.dump());
    }
    return results;
}

} // namespace inference
