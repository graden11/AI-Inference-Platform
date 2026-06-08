#include "../include/RemoteBackend.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <muduo/base/Logging.h>
#include <sstream>
#include <stdexcept>

namespace inference {

// ---------------------------------------------------------------------------
// curl helper — write callback
// ---------------------------------------------------------------------------
static size_t writeCallback(void* contents, size_t size, size_t nmemb, std::string* out)
{
    size_t total = size * nmemb;
    out->append(static_cast<const char*>(contents), total);
    return total;
}

// ---------------------------------------------------------------------------
// Serialize tensor to JSON
// ---------------------------------------------------------------------------
static nlohmann::json serializeRequest(const std::vector<float>& input,
                                        const std::vector<int64_t>& inputShape,
                                        int batchSize)
{
    nlohmann::json req;
    req["input"] = input;
    req["input_shape"] = inputShape;
    req["batch_size"] = batchSize;
    return req;
}

// ---------------------------------------------------------------------------
// Parse tensor response from JSON
// ---------------------------------------------------------------------------
static InferenceOutput parseResponse(const nlohmann::json& resp)
{
    InferenceOutput out;

    if (resp.contains("data") && resp["data"].is_array())
    {
        out.data = resp["data"].get<std::vector<float>>();
    }
    if (resp.contains("shape") && resp["shape"].is_array())
    {
        for (const auto& d : resp["shape"])
            out.shape.push_back(d.get<int64_t>());
    }

    // Multi-output: shapes + tensors arrays
    if (resp.contains("shapes") && resp["shapes"].is_array())
    {
        for (const auto& s : resp["shapes"])
        {
            std::vector<int64_t> dims;
            for (const auto& d : s)
                dims.push_back(d.get<int64_t>());
            out.shapes.push_back(std::move(dims));
        }
    }
    if (resp.contains("tensors") && resp["tensors"].is_array())
    {
        for (const auto& t : resp["tensors"])
            out.tensors.push_back(t.get<std::vector<float>>());
    }
    if (resp.contains("names") && resp["names"].is_array())
    {
        for (const auto& n : resp["names"])
            out.names.push_back(n.get<std::string>());
    }

    return out;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
RemoteBackend::RemoteBackend(const ModelConfig& config)
    : maxBatchSize_(config.max_batch_size > 0 ? config.max_batch_size : 8)
    , timeoutMs_(30000)
{
    // remote_url can come from config path field or a dedicated field.
    // We use config.path as the remote URL when type is "remote".
    remoteUrl_ = config.path;
    if (remoteUrl_.empty())
        throw std::runtime_error("RemoteBackend: config.path (remote URL) is required");

    // Ensure URL doesn't end with slash
    while (!remoteUrl_.empty() && remoteUrl_.back() == '/')
        remoteUrl_.pop_back();

    LOG_INFO << "RemoteBackend initialized, target: " << remoteUrl_
             << ", maxBatchSize: " << maxBatchSize_;
}

// ---------------------------------------------------------------------------
// Core: send tensor to remote GPU, get tensor back
// ---------------------------------------------------------------------------
InferenceOutput RemoteBackend::remoteCall(const std::vector<float>& input,
                                           const std::vector<int64_t>& inputShape,
                                           int batchSize)
{
    CURL* curl = curl_easy_init();
    if (!curl)
        throw std::runtime_error("RemoteBackend: curl_easy_init failed");

    std::string url = remoteUrl_ + "/predict/tensor";
    nlohmann::json reqBody = serializeRequest(input, inputShape, batchSize);
    std::string reqStr = reqBody.dump();
    std::string responseStr;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, reqStr.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(reqStr.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseStr);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeoutMs_));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK)
    {
        std::string err = curl_easy_strerror(res);
        LOG_ERROR << "RemoteBackend: curl error to " << url << ": " << err;
        throw std::runtime_error("RemoteBackend: HTTP request failed: " + err);
    }

    if (httpCode != 200)
    {
        LOG_ERROR << "RemoteBackend: HTTP " << httpCode << " from " << url;
        throw std::runtime_error("RemoteBackend: HTTP " + std::to_string(httpCode));
    }

    try
    {
        auto resp = nlohmann::json::parse(responseStr);
        if (resp.value("status", "") == "error")
        {
            throw std::runtime_error("RemoteBackend: remote error: " +
                                     resp.value("message", std::string("unknown")));
        }
        return parseResponse(resp);
    }
    catch (const nlohmann::json::exception& e)
    {
        LOG_ERROR << "RemoteBackend: JSON parse error: " << e.what();
        throw std::runtime_error("RemoteBackend: JSON parse failed");
    }
}

// ---------------------------------------------------------------------------
// Single-sample inference
// ---------------------------------------------------------------------------
std::vector<float> RemoteBackend::infer(const std::vector<float>& input,
                                         const std::vector<int64_t>& inputShape,
                                         std::vector<int64_t>& outputShape)
{
    auto out = remoteCall(input, inputShape, 1);
    outputShape = out.shape;
    return out.data.empty() ? std::vector<float>(out.dataPtrOrCopy(),
                                                  out.dataPtrOrCopy() + out.totalElements())
                            : out.data;
}

InferenceOutput RemoteBackend::inferMulti(const std::vector<float>& input,
                                           const std::vector<int64_t>& inputShape)
{
    return remoteCall(input, inputShape, 1);
}

// ---------------------------------------------------------------------------
// Batch inference
// ---------------------------------------------------------------------------
std::vector<float> RemoteBackend::inferBatch(const std::vector<float>& batchInput,
                                              const std::vector<int64_t>& batchShape,
                                              std::vector<int64_t>& outputShape)
{
    int batchSize = static_cast<int>(batchShape[0]);
    auto out = remoteCall(batchInput, batchShape, batchSize);
    outputShape = out.shape;
    return out.data.empty() ? std::vector<float>(out.dataPtrOrCopy(),
                                                  out.dataPtrOrCopy() + out.totalElements())
                            : out.data;
}

InferenceOutput RemoteBackend::inferBatchMulti(const std::vector<float>& batchInput,
                                                const std::vector<int64_t>& batchShape)
{
    int batchSize = static_cast<int>(batchShape[0]);
    return remoteCall(batchInput, batchShape, batchSize);
}

// ---------------------------------------------------------------------------
// Health check
// ---------------------------------------------------------------------------
bool RemoteBackend::isReady() const
{
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = remoteUrl_ + "/health";
    std::string responseStr;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseStr);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    return res == CURLE_OK && httpCode == 200;
}

} // namespace inference
