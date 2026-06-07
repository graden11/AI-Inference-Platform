#include "../../include/handlers/RawPredictHandler.h"
#include "../../include/ModelFactory.h"
#include "../../include/InferenceEngine.h"
#include "../../include/RequestBatcher.h"

#include "../../../../HttpServer/include/utils/MetricsCollector.h"

#include <chrono>
#include <muduo/base/Logging.h>

namespace
{

void sendRawError(const http::HttpRequest &req, http::HttpResponse *resp,
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

void RawPredictHandler::handle(const http::HttpRequest &req, http::HttpResponse *resp)
{
    try
    {
        std::string modelName = req.getQueryParameters("model_name");
        if (modelName.empty())
            modelName = "resnet50";

        const std::string& body = req.getBody();
        if (body.empty())
        {
            sendRawError(req, resp, http::HttpResponse::k400BadRequest, "empty body");
            return;
        }

        std::string ct = req.getHeader("Content-Type");
        if (ct.find("image/") == std::string::npos)
        {
            sendRawError(req, resp, http::HttpResponse::k400BadRequest,
                         "Content-Type must be an image type (image/jpeg, image/png, etc.)");
            return;
        }

        std::vector<uint8_t> imageBytes(body.begin(), body.end());

        // Batching path
        if (batcher_)
        {
            auto future = batcher_->submit(modelName, std::move(imageBytes));
            std::string resultJson = future.get();

            resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
            resp->setContentType("application/json");
            resp->setContentLength(resultJson.size());
            resp->setBody(std::move(resultJson));
            resp->setCloseConnection(false);
            return;
        }

        // Direct path
        auto engine = factory_->getModel(modelName);
        if (!engine)
        {
            sendRawError(req, resp, http::HttpResponse::k400BadRequest,
                         "unknown model: " + modelName);
            return;
        }

        std::string resultJson = engine->predictFromBytes(imageBytes);

        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setContentType("application/json");
        resp->setContentLength(resultJson.size());
        resp->setBody(std::move(resultJson));
        resp->setCloseConnection(false);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR << "RawPredictHandler error: " << e.what();
        sendRawError(req, resp, http::HttpResponse::k500InternalServerError,
                     std::string("internal error: ") + e.what());
    }
    catch (...)
    {
        LOG_ERROR << "RawPredictHandler unknown error";
        sendRawError(req, resp, http::HttpResponse::k500InternalServerError, "internal error");
    }
}
