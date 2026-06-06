#pragma once

#include "InferenceEngine.h"

#include "InferenceBackend.h"
#include "ModelConfig.h"
#include "Postprocessor.h"
#include "Preprocessor.h"

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace inference {

/// Composes Preprocessor + InferenceBackend + Postprocessor.
/// Inherits InferenceEngine for seamless drop-in with ModelFactory and handlers.
class ModelPipeline : public InferenceEngine
{
public:
    ModelPipeline(ModelConfig config,
                  std::unique_ptr<Preprocessor> preprocessor,
                  std::unique_ptr<InferenceBackend> backend,
                  std::unique_ptr<Postprocessor> postprocessor);

    // --- InferenceEngine interface ---
    std::string predict(const std::string& imagePath) override;
    std::string predictFromBytes(const std::vector<uint8_t>& imageData) override;
    int maxBatchSize() const override;
    std::vector<std::string> predictBatch(const std::vector<std::vector<uint8_t>>& images) override;

    // --- Extended API ---

    /// Like predictFromBytes but returns structured JSON (avoids string round-trip
    /// for handlers that need to populate protobuf fields, etc.)
    nlohmann::json predictFromBytesJson(const std::vector<uint8_t>& imageData);

    /// Accessors
    const ModelConfig& config() const { return config_; }
    const std::vector<std::string>& labels() const { return labels_; }

private:
    nlohmann::json doPredictJson(const std::vector<uint8_t>& imageBytes);

    ModelConfig config_;
    std::unique_ptr<Preprocessor> preprocessor_;
    std::unique_ptr<InferenceBackend> backend_;
    std::unique_ptr<Postprocessor> postprocessor_;
    std::vector<std::string> labels_;
};

} // namespace inference
