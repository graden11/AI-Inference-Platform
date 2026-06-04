#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace inference {

enum class TaskType {
    CLASSIFICATION,
    DETECTION,
    SEGMENTATION,
};

inline TaskType parseTaskType(const std::string& s)
{
    if (s == "classification") return TaskType::CLASSIFICATION;
    if (s == "detection")      return TaskType::DETECTION;
    if (s == "segmentation")   return TaskType::SEGMENTATION;
    return TaskType::CLASSIFICATION; // default
}

inline const char* taskTypeToString(TaskType t)
{
    switch (t) {
        case TaskType::CLASSIFICATION: return "classification";
        case TaskType::DETECTION:      return "detection";
        case TaskType::SEGMENTATION:   return "segmentation";
    }
    return "unknown";
}

/// I/O tensor configuration for a model.
struct TensorIOConfig {
    std::string name;
    std::vector<int64_t> shape;   // e.g. {1, 3, 224, 224} — dim[0] is batch
};

/// Input-specific configuration including preprocessing parameters.
struct InputConfig : TensorIOConfig {
    int preferred_width  = 224;
    int preferred_height = 224;
    int channels         = 3;
    std::string layout   = "chw";  // "chw" (PyTorch) or "hwc" (TensorFlow/Keras)
    std::vector<float> mean = {0.485f, 0.456f, 0.406f};
    std::vector<float> std  = {0.229f, 0.224f, 0.225f};

    int elemCount() const { return channels * preferred_height * preferred_width; }
};

/// Output-specific configuration.
struct OutputConfig : TensorIOConfig {
    std::string layout = "chw";  // "chw" or "hwc"
};

/// Complete metadata for one model version.
struct ModelConfig {
    std::string name;
    std::string version;
    std::string type;       // backend identifier: "onnx", "tensorrt", ...
    std::string path;       // model file path
    TaskType    task = TaskType::CLASSIFICATION;
    std::string labels_path;  // per-model labels; empty = use global fallback
    int         top_k = 5;
    int         max_batch_size = 1;

    // Detection
    float confidence_threshold = 0.5f;
    float nms_threshold = 0.45f;
    int   max_detections = 100;

    // Segmentation
    int   num_segmentation_classes = 0; // 0 = auto-detect from output shape

    InputConfig  input;
    OutputConfig output;

    // --- factories ---

    /// Build from the new rich JSON format (with input/output subsections).
    /// Missing fields fall back to ImageNet defaults (backward-compatible).
    static ModelConfig fromJson(const std::string& name,
                                const std::string& version,
                                const nlohmann::json& entry,
                                const std::string& globalLabelsPath = "",
                                int defaultBatchSize = 1);

    /// Convenience: build from flat parameters (old config format or handler params).
    static ModelConfig simple(const std::string& name,
                              const std::string& version,
                              const std::string& type,
                              const std::string& path,
                              const std::string& labelsPath = "",
                              int batchSize = 1);
};

} // namespace inference
