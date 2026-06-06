#include "../../include/handlers/PredictHandler.h"
#include "../../include/ModelFactory.h"
#include "../../include/InferenceEngine.h"
#include "../../include/RequestBatcher.h"

#include "../../../../HttpServer/include/utils/PathValidator.h"
#include "../../../../HttpServer/include/utils/Base64.h"

#include <muduo/base/Logging.h>

#include <fstream>
#include <utility>
#include <vector>

namespace
{

const std::vector<std::string> kAllowedReadDirs = {"models", "images"};

std::vector<uint8_t> readFile(const std::string &path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return {};
    auto size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    f.read(reinterpret_cast<char *>(data.data()), size);
    return data;
}

void sendPredictError(const http::HttpRequest &req, http::HttpResponse *resp,
                      http::HttpResponse::HttpStatusCode code,
                      const std::string &message)
{
    json err;
    err["status"] = "error";
    err["message"] = message;
    std::string errBody = err.dump();
    resp->setStatusLine(req.getVersion(), code,
        code == http::HttpResponse::k400BadRequest ? "Bad Request" : "Internal Server Error");
    resp->setContentType("application/json");
    resp->setContentLength(errBody.size());
    resp->setBody(std::move(errBody));
    resp->setCloseConnection(code != http::HttpResponse::k200Ok);
}

} // anonymous namespace

void PredictHandler::handle(const http::HttpRequest &req, http::HttpResponse *resp)
{
    try
    {
        json body = json::parse(req.getBody());

        bool hasPath = body.contains("image_path");
        bool hasData = body.contains("image_data");

        if (!hasPath && !hasData)
        {
            sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                             "missing image_path or image_data");
            return;
        }

        std::string modelName = body.value("model_name", "resnet50");

        // Batching path
        if (batcher_)
        {
            std::vector<uint8_t> imageBytes;
            if (hasPath)
            {
                const auto& imagePath = body["image_path"].get_ref<const std::string&>();
                if (!http::utils::isPathSafeInDirs(imagePath, kAllowedReadDirs))
                {
                    sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                                     "image_path is outside allowed directories");
                    return;
                }
                imageBytes = readFile(imagePath);
                if (imageBytes.empty())
                {
                    sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                                     "failed to read image file");
                    return;
                }
            }
            else
            {
                const auto& b64 = body["image_data"].get_ref<const std::string&>();
                imageBytes = http::utils::base64Decode(b64);
                if (imageBytes.empty())
                {
                    sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                                     "failed to decode base64 image");
                    return;
                }
            }

            auto future = batcher_->submit(modelName, std::move(imageBytes));
            std::string resultJson = future.get();

            resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
            resp->setContentType("application/json");
            resp->setContentLength(resultJson.size());
            resp->setBody(std::move(resultJson));
            resp->setCloseConnection(false);
            return;
        }

        // Direct path (no batching)
        auto engine = factory_->getModel(modelName);
        if (!engine)
        {
            sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                             "unknown model: " + modelName);
            return;
        }

        std::string resultJson;
        if (hasPath)
        {
            const auto& imagePath = body["image_path"].get_ref<const std::string&>();
            if (!http::utils::isPathSafeInDirs(imagePath, kAllowedReadDirs))
            {
                sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                                 "image_path is outside allowed directories");
                return;
            }
            resultJson = engine->predict(imagePath);
        }
        else
        {
            const auto& b64 = body["image_data"].get_ref<const std::string&>();
            auto imageBytes = http::utils::base64Decode(b64);
            if (imageBytes.empty())
            {
                sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                                 "failed to decode base64 image");
                return;
            }
            resultJson = engine->predictFromBytes(imageBytes);
        }

        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setContentType("application/json");
        resp->setContentLength(resultJson.size());
        resp->setBody(std::move(resultJson));
        resp->setCloseConnection(false);
    }
    catch (const json::exception &e)
    {
        LOG_ERROR << "PredictHandler JSON parse error: " << e.what();
        sendPredictError(req, resp, http::HttpResponse::k400BadRequest,
                         std::string("invalid JSON: ") + e.what());
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "PredictHandler error: " << e.what();
        sendPredictError(req, resp, http::HttpResponse::k500InternalServerError,
                         std::string("internal error: ") + e.what());
    }
    catch (...)
    {
        LOG_ERROR << "PredictHandler unknown error";
        sendPredictError(req, resp, http::HttpResponse::k500InternalServerError,
                         "internal error");
    }
}
