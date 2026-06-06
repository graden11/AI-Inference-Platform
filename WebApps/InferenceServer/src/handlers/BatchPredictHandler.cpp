#include "../../include/handlers/BatchPredictHandler.h"
#include "../../include/ModelFactory.h"
#include "../../include/InferenceEngine.h"

#include <muduo/base/Logging.h>

#include "../../../../HttpServer/include/utils/JsonUtil.h"
#include "../../../../HttpServer/include/utils/PathValidator.h"
#include "../../../../HttpServer/include/utils/Base64.h"

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

void sendError(const http::HttpRequest &req, http::HttpResponse *resp,
               http::HttpResponse::HttpStatusCode code,
               const std::string &message)
{
    json err;
    err["status"] = "error";
    err["message"] = message;
    std::string body = err.dump();
    resp->setStatusLine(req.getVersion(), code,
        code == http::HttpResponse::k400BadRequest ? "Bad Request" : "Internal Server Error");
    resp->setContentType("application/json");
    resp->setContentLength(body.size());
    resp->setBody(std::move(body));
    resp->setCloseConnection(false);
}

} // anonymous namespace

void BatchPredictHandler::handle(const http::HttpRequest &req, http::HttpResponse *resp)
{
    try
    {
        json body = json::parse(req.getBody());

        bool hasPaths = body.contains("image_paths");
        bool hasData  = body.contains("images");

        if (!hasPaths && !hasData)
        {
            sendError(req, resp, http::HttpResponse::k400BadRequest,
                      "missing 'images' (base64 array) or 'image_paths' (path array)");
            return;
        }

        if (hasPaths && hasData)
        {
            sendError(req, resp, http::HttpResponse::k400BadRequest,
                      "use either 'images' or 'image_paths', not both");
            return;
        }

        std::string modelName = body.value("model_name", "resnet50");

        auto engine = factory_->getModel(modelName);
        if (!engine)
        {
            sendError(req, resp, http::HttpResponse::k400BadRequest,
                      "unknown model: " + modelName);
            return;
        }

        std::vector<std::vector<uint8_t>> imageBytes;

        if (hasPaths)
        {
            auto paths = body["image_paths"];
            if (!paths.is_array() || paths.empty())
            {
                sendError(req, resp, http::HttpResponse::k400BadRequest,
                          "'image_paths' must be a non-empty array");
                return;
            }
            imageBytes.reserve(paths.size());

            for (auto &p : paths)
            {
                const auto& pathStr = p.get_ref<const std::string&>();
                if (!http::utils::isPathSafeInDirs(pathStr, kAllowedReadDirs))
                {
                    sendError(req, resp, http::HttpResponse::k400BadRequest,
                              "image_path is outside allowed directories: " + pathStr);
                    return;
                }
                auto data = readFile(pathStr);
                if (data.empty())
                {
                    sendError(req, resp, http::HttpResponse::k400BadRequest,
                              "failed to read image: " + pathStr);
                    return;
                }
                imageBytes.push_back(std::move(data));
            }
        }
        else
        {
            auto images = body["images"];
            if (!images.is_array() || images.empty())
            {
                sendError(req, resp, http::HttpResponse::k400BadRequest,
                          "'images' must be a non-empty array");
                return;
            }
            imageBytes.reserve(images.size());

            for (auto &img : images)
            {
                const auto& b64 = img.get_ref<const std::string&>();
                auto data = http::utils::base64Decode(b64);
                if (data.empty())
                {
                    sendError(req, resp, http::HttpResponse::k400BadRequest,
                              "failed to decode base64 image at index");
                    return;
                }
                imageBytes.push_back(std::move(data));
            }
        }

        std::vector<std::string> resultJsons;
        try
        {
            resultJsons = engine->predictBatch(imageBytes);
        }
        catch (const std::exception &e)
        {
            LOG_ERROR << "BatchPredictHandler predictBatch error: " << e.what();
            sendError(req, resp, http::HttpResponse::k500InternalServerError,
                      std::string("inference error: ") + e.what());
            return;
        }

        json response;
        response["status"] = "ok";
        response["model_name"] = modelName;
        response["count"] = static_cast<int>(resultJsons.size());

        json results = json::array();
        for (auto &r : resultJsons)
        {
            try
            {
                results.push_back(json::parse(r));
            }
            catch (...)
            {
                results.push_back(r);
            }
        }
        response["results"] = results;

        std::string respBody = response.dump();
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setContentType("application/json");
        resp->setContentLength(respBody.size());
        resp->setBody(std::move(respBody));
        resp->setCloseConnection(false);
    }
    catch (const json::exception &e)
    {
        LOG_ERROR << "BatchPredictHandler JSON parse error: " << e.what();
        sendError(req, resp, http::HttpResponse::k400BadRequest,
                  std::string("invalid JSON: ") + e.what());
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "BatchPredictHandler error: " << e.what();
        sendError(req, resp, http::HttpResponse::k500InternalServerError,
                  std::string("internal error: ") + e.what());
    }
    catch (...)
    {
        LOG_ERROR << "BatchPredictHandler unknown error";
        sendError(req, resp, http::HttpResponse::k500InternalServerError,
                  "internal error");
    }
}
