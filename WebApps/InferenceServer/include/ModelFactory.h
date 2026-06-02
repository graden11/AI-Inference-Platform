#pragma once

#include "InferenceEngine.h"

#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

class ModelFactory
{
public:
    struct ModelInfo {
        std::string name;
        std::string version;
        std::string type;
        std::string path;
        std::string task;
        std::string labels;
        int top_k = 5;
        int input_width = 224;
        int input_height = 224;
        int input_channels = 3;
        std::string input_name = "input";
        std::string output_name = "output";
        std::vector<float> input_mean = {0.485f, 0.456f, 0.406f};
        std::vector<float> input_std  = {0.229f, 0.224f, 0.225f};
        float confidence_threshold = 0.5f;
        float nms_threshold = 0.45f;
        int   max_detections = 100;
        bool is_latest = false;
    };

    // Internal full metadata struct (public for use by saveConfig / loadModel)
    struct ModelMeta {
        std::string type;
        std::string path;
        std::string task = "classification";
        std::string labels;
        int top_k = 5;
        int input_width = 224;
        int input_height = 224;
        int input_channels = 3;
        std::string input_name = "input";
        std::string output_name = "output";
        std::vector<float> input_mean = {0.485f, 0.456f, 0.406f};
        std::vector<float> input_std  = {0.229f, 0.224f, 0.225f};
        float confidence_threshold = 0.5f;
        float nms_threshold = 0.45f;
        int   max_detections = 100;
    };

    // Parse "name:version" into (name, version). Empty version means "latest".
    static std::pair<std::string, std::string> parseModelName(const std::string& spec);

    // Compare version strings (supports "v1", "v2.3", "10" etc.)
    static int compareVersions(const std::string& a, const std::string& b);

    ModelFactory() = default;

    // Register a versioned model. Takes ownership of engine.
    void registerModel(const std::string& name,
                       const std::string& version,
                       std::shared_ptr<InferenceEngine> engine,
                       const std::string& type,
                       const std::string& path);

    // Get engine by "name:version" or "name" (returns latest version).
    std::shared_ptr<InferenceEngine> getModel(const std::string& nameAndVersion) const;

    // Get specific version
    std::shared_ptr<InferenceEngine> getModel(const std::string& name,
                                              const std::string& version) const;

    // Check if a model exists (no version = check if any version exists)
    bool hasModel(const std::string& name) const;
    bool hasModel(const std::string& name, const std::string& version) const;

    // Unload a specific version. Returns false if not found.
    // The engine will be destroyed when the last shared_ptr reference is released.
    bool unloadModel(const std::string& name, const std::string& version);

    // List all loaded models
    std::vector<ModelInfo> listModels() const;

    // Get latest version string for a name
    std::string getLatestVersion(const std::string& name) const;

    // Get model metadata (type and path)
    std::string getModelType(const std::string& name, const std::string& version) const;
    std::string getModelPath(const std::string& name, const std::string& version) const;

    // Number of loaded model versions (for config serialization)
    size_t modelCount() const;

    // Get full metadata for a model (used by saveConfig)
    const ModelMeta* getFullMeta(const std::string& name, const std::string& version) const;

private:
    // name -> (version -> shared_ptr<InferenceEngine>)
    std::unordered_map<std::string,
        std::map<std::string, std::shared_ptr<InferenceEngine>>> models_;
    // name -> latest version string
    std::unordered_map<std::string, std::string> latestVersions_;
    // name -> (version -> {type, path, task})
    std::unordered_map<std::string,
        std::map<std::string, std::tuple<std::string, std::string, std::string>>> metadata_;
    // name -> (version -> full metadata for persistence)
    std::unordered_map<std::string,
        std::map<std::string, ModelMeta>> fullMeta_;

    mutable std::shared_mutex mutex_;
};
