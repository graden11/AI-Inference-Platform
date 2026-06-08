#include "../include/BackendRegistry.h"
#include "../include/OnnxBackend.h"
#include "../include/RemoteBackend.h"
#ifdef ENABLE_TENSORRT
#include "../include/TRTBackend.h"
#endif

namespace inference {

BackendRegistry& BackendRegistry::instance()
{
    static BackendRegistry reg;
    return reg;
}

void BackendRegistry::registerBackend(const std::string& type, FactoryFunc factory)
{
    factories_[type] = std::move(factory);
}

std::unique_ptr<InferenceBackend> BackendRegistry::create(const std::string& type,
                                                           const ModelConfig& config)
{
    auto it = factories_.find(type);
    if (it == factories_.end())
        return nullptr;
    return it->second(config);
}

bool BackendRegistry::has(const std::string& type) const
{
    return factories_.find(type) != factories_.end();
}

std::vector<std::string> BackendRegistry::list() const
{
    std::vector<std::string> result;
    result.reserve(factories_.size());
    for (auto& [name, _] : factories_)
        result.push_back(name);
    return result;
}

// ---------------------------------------------------------------------------
// Static auto-registration of built-in backends
// ---------------------------------------------------------------------------
namespace {

struct AutoRegisterBuiltins {
    AutoRegisterBuiltins() {
        auto& reg = BackendRegistry::instance();

        reg.registerBackend("onnx", [](const ModelConfig& cfg) {
            return std::make_unique<OnnxBackend>(cfg);
        });

        // Remote backend: forwards tensor to GPU server over HTTP.
        // config.path must be the remote URL (e.g. http://127.0.0.1:9080).
        reg.registerBackend("remote", [](const ModelConfig& cfg) {
            return std::make_unique<RemoteBackend>(cfg);
        });

#ifdef ENABLE_TENSORRT
        reg.registerBackend("tensorrt", [](const ModelConfig& cfg) {
            return std::make_unique<TRTBackend>(cfg);
        });
#endif
    }
};

static AutoRegisterBuiltins _autoRegister;

} // anonymous namespace
} // namespace inference
