#include "../include/ModelFactory.h"
#include "../include/ModelPipeline.h"
#include <algorithm>
#include <mutex>
#include <sstream>

std::pair<std::string, std::string> ModelFactory::parseModelName(const std::string& spec)
{
    auto pos = spec.rfind(':');
    if (pos == std::string::npos)
        return {spec, ""};
    return {spec.substr(0, pos), spec.substr(pos + 1)};
}

int ModelFactory::compareVersions(const std::string& a, const std::string& b)
{
    auto parseComponents = [](const std::string& s) -> std::vector<int> {
        std::vector<int> parts;
        std::string num;
        for (char c : s) {
            if (c >= '0' && c <= '9') {
                num += c;
            } else if (!num.empty()) {
                parts.push_back(std::stoi(num));
                num.clear();
            }
        }
        if (!num.empty())
            parts.push_back(std::stoi(num));
        return parts;
    };

    auto va = parseComponents(a);
    auto vb = parseComponents(b);
    size_t n = std::max(va.size(), vb.size());
    for (size_t i = 0; i < n; ++i) {
        int x = i < va.size() ? va[i] : 0;
        int y = i < vb.size() ? vb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

// --- Helpers using ModelFactory::ModelMeta ---

static ModelFactory::ModelMeta extractMeta(const std::string& type, const std::string& path,
                              std::shared_ptr<InferenceEngine> engine)
{
    ModelFactory::ModelMeta m;
    m.type = type;
    m.path = path;
    m.task = "classification";
    auto* pipeline = dynamic_cast<inference::ModelPipeline*>(engine.get());
    if (pipeline) {
        const auto& cfg = pipeline->config();
        m.task        = inference::taskTypeToString(cfg.task);
        m.labels      = cfg.labels_path;
        m.top_k       = cfg.top_k;
        m.input_width  = cfg.input.preferred_width;
        m.input_height = cfg.input.preferred_height;
        m.input_channels = cfg.input.channels;
        m.input_name   = cfg.input.name;
        m.output_name  = cfg.output.name;
        m.input_mean   = cfg.input.mean;
        m.input_std    = cfg.input.std;
        m.confidence_threshold = cfg.confidence_threshold;
        m.nms_threshold = cfg.nms_threshold;
        m.max_detections = cfg.max_detections;
    }
    return m;
}

void ModelFactory::registerModel(const std::string& name,
                                  const std::string& version,
                                  std::shared_ptr<InferenceEngine> engine,
                                  const std::string& type,
                                  const std::string& path)
{
    std::unique_lock lock(mutex_);
    models_[name][version] = std::move(engine);
    metadata_[name][version] = {type, path, "classification"};
    fullMeta_[name][version] = extractMeta(type, path, models_[name][version]);

    // Update latest version
    auto it = latestVersions_.find(name);
    if (it == latestVersions_.end() || compareVersions(version, it->second) > 0) {
        latestVersions_[name] = version;
    }
}

std::shared_ptr<InferenceEngine> ModelFactory::getModel(const std::string& nameAndVersion) const
{
    auto [name, version] = parseModelName(nameAndVersion);
    if (version.empty())
        return getModel(name, getLatestVersion(name));
    return getModel(name, version);
}

std::shared_ptr<InferenceEngine> ModelFactory::getModel(const std::string& name,
                                                         const std::string& version) const
{
    std::shared_lock lock(mutex_);
    auto nameIt = models_.find(name);
    if (nameIt == models_.end()) return nullptr;
    auto verIt = nameIt->second.find(version);
    if (verIt == nameIt->second.end()) return nullptr;
    return verIt->second;
}

bool ModelFactory::hasModel(const std::string& name) const
{
    std::shared_lock lock(mutex_);
    return models_.find(name) != models_.end();
}

bool ModelFactory::hasModel(const std::string& name, const std::string& version) const
{
    std::shared_lock lock(mutex_);
    auto it = models_.find(name);
    if (it == models_.end()) return false;
    return it->second.find(version) != it->second.end();
}

bool ModelFactory::unloadModel(const std::string& name, const std::string& version)
{
    std::unique_lock lock(mutex_);
    auto nameIt = models_.find(name);
    if (nameIt == models_.end()) return false;
    auto& versions = nameIt->second;
    auto verIt = versions.find(version);
    if (verIt == versions.end()) return false;

    versions.erase(verIt);
    metadata_[name].erase(version);
    fullMeta_[name].erase(version);

    // Recompute latest version
    if (versions.empty()) {
        models_.erase(name);
        metadata_.erase(name);
        fullMeta_.erase(name);
        latestVersions_.erase(name);
    } else {
        std::string newLatest;
        for (auto& [v, _] : versions) {
            if (newLatest.empty() || compareVersions(v, newLatest) > 0)
                newLatest = v;
        }
        latestVersions_[name] = newLatest;
    }

    return true;
}

std::vector<ModelFactory::ModelInfo> ModelFactory::listModels() const
{
    std::shared_lock lock(mutex_);
    std::vector<ModelInfo> result;
    for (auto& [name, versions] : models_) {
        for (auto& [version, _] : versions) {
            ModelInfo info;
            info.name = name;
            info.version = version;
            info.is_latest = (version == getLatestVersion(name));
            auto metaIt = fullMeta_.find(name);
            if (metaIt != fullMeta_.end()) {
                auto verMetaIt = metaIt->second.find(version);
                if (verMetaIt != metaIt->second.end()) {
                    const auto& m = verMetaIt->second;
                    info.type = m.type;
                    info.path = m.path;
                    info.task = m.task;
                    info.labels = m.labels;
                    info.top_k = m.top_k;
                    info.input_width  = m.input_width;
                    info.input_height = m.input_height;
                    info.input_channels = m.input_channels;
                    info.input_name   = m.input_name;
                    info.output_name  = m.output_name;
                    info.input_mean   = m.input_mean;
                    info.input_std    = m.input_std;
                    info.confidence_threshold = m.confidence_threshold;
                    info.nms_threshold = m.nms_threshold;
                    info.max_detections = m.max_detections;
                }
            }
            result.push_back(info);
        }
    }
    return result;
}

const ModelFactory::ModelMeta* ModelFactory::getFullMeta(const std::string& name,
                                                          const std::string& version) const
{
    std::shared_lock lock(mutex_);
    auto it = fullMeta_.find(name);
    if (it == fullMeta_.end()) return nullptr;
    auto vit = it->second.find(version);
    return vit != it->second.end() ? &vit->second : nullptr;
}

std::string ModelFactory::getLatestVersion(const std::string& name) const
{
    std::shared_lock lock(mutex_);
    auto it = latestVersions_.find(name);
    return it != latestVersions_.end() ? it->second : "";
}

std::string ModelFactory::getModelType(const std::string& name, const std::string& version) const
{
    std::shared_lock lock(mutex_);
    auto it = metadata_.find(name);
    if (it == metadata_.end()) return "";
    auto vit = it->second.find(version);
    return vit != it->second.end() ? std::get<0>(vit->second) : "";
}

std::string ModelFactory::getModelPath(const std::string& name, const std::string& version) const
{
    std::shared_lock lock(mutex_);
    auto it = metadata_.find(name);
    if (it == metadata_.end()) return "";
    auto vit = it->second.find(version);
    return vit != it->second.end() ? std::get<1>(vit->second) : "";
}

size_t ModelFactory::modelCount() const
{
    std::shared_lock lock(mutex_);
    size_t count = 0;
    for (auto& [_, versions] : models_)
        count += versions.size();
    return count;
}
