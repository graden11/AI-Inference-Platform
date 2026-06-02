#include "../include/ModelFactory.h"
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

void ModelFactory::registerModel(const std::string& name,
                                  const std::string& version,
                                  std::shared_ptr<InferenceEngine> engine,
                                  const std::string& type,
                                  const std::string& path)
{
    std::unique_lock lock(mutex_);
    models_[name][version] = std::move(engine);
    metadata_[name][version] = {type, path};

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

    // Recompute latest version
    if (versions.empty()) {
        models_.erase(name);
        metadata_.erase(name);
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
            auto metaIt = metadata_.find(name);
            if (metaIt != metadata_.end()) {
                auto typeIt = metaIt->second.find(version);
                if (typeIt != metaIt->second.end()) {
                    info.type = typeIt->second.first;
                    info.path = typeIt->second.second;
                }
            }
            result.push_back(info);
        }
    }
    return result;
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
    return vit != it->second.end() ? vit->second.first : "";
}

std::string ModelFactory::getModelPath(const std::string& name, const std::string& version) const
{
    std::shared_lock lock(mutex_);
    auto it = metadata_.find(name);
    if (it == metadata_.end()) return "";
    auto vit = it->second.find(version);
    return vit != it->second.end() ? vit->second.second : "";
}

size_t ModelFactory::modelCount() const
{
    std::shared_lock lock(mutex_);
    size_t count = 0;
    for (auto& [_, versions] : models_)
        count += versions.size();
    return count;
}
