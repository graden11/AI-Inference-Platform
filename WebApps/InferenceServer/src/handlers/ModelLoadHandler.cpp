#include "../../include/handlers/ModelLoadHandler.h"
#include "../../include/InferenceServer.h"
#include "../../include/ModelFactory.h"
#include "../../include/ResNet50Engine.h"
#ifdef ENABLE_TENSORRT
#include "../../include/ResNet50TRTEngine.h"
#endif

#include <muduo/base/Logging.h>
#include <fstream>

void ModelLoadHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    try
    {
        auto session = server_->getSessionManager()->getSession(req, resp);
        if (session->getValue("isLoggedIn") != "true")
        {
            json err;
            err["status"] = "error";
            err["message"] = "Unauthorized";
            std::string errBody = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k401Unauthorized, "Unauthorized");
            resp->setContentType("application/json");
            resp->setContentLength(errBody.size());
            resp->setBody(errBody);
            resp->setCloseConnection(true);
            return;
        }

        json body = json::parse(req.getBody());

        std::string name = body.value("name", "");
        std::string version = body.value("version", "1");
        std::string type = body.value("type", "onnx");
        std::string path = body.value("path", "");

        if (name.empty() || path.empty())
        {
            json err;
            err["status"] = "error";
            err["message"] = "name and path are required";
            std::string errBody = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
            resp->setContentType("application/json");
            resp->setContentLength(errBody.size());
            resp->setBody(errBody);
            resp->setCloseConnection(false);
            return;
        }

        auto* factory = server_->getModelFactory();
        if (!factory)
        {
            json err;
            err["status"] = "error";
            err["message"] = "ModelFactory not initialized";
            std::string errBody = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k500InternalServerError, "Internal Server Error");
            resp->setContentType("application/json");
            resp->setContentLength(errBody.size());
            resp->setBody(errBody);
            resp->setCloseConnection(true);
            return;
        }

        // Check if already loaded
        if (factory->hasModel(name, version))
        {
            json err;
            err["status"] = "error";
            err["message"] = "model already loaded: " + name + ":" + version;
            std::string errBody = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k409Conflict, "Conflict");
            resp->setContentType("application/json");
            resp->setContentLength(errBody.size());
            resp->setBody(errBody);
            resp->setCloseConnection(false);
            return;
        }

        // Check file exists
        if (!std::ifstream(path).good())
        {
            json err;
            err["status"] = "error";
            err["message"] = "model file not found: " + path;
            std::string errBody = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
            resp->setContentType("application/json");
            resp->setContentLength(errBody.size());
            resp->setBody(errBody);
            resp->setCloseConnection(false);
            return;
        }

        const std::string& labelsPath = server_->getLabelsPath();
        int batchSize = server_->config_.batching.enabled ? server_->config_.batching.max_batch_size : 1;

        if (type == "tensorrt")
        {
#ifdef ENABLE_TENSORRT
            factory->registerModel(name, version,
                std::make_shared<ResNet50TRTEngine>(path, labelsPath, batchSize), type, path);
#else
            json err;
            err["status"] = "error";
            err["message"] = "TensorRT support disabled";
            std::string errBody = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
            resp->setContentType("application/json");
            resp->setContentLength(errBody.size());
            resp->setBody(errBody);
            resp->setCloseConnection(false);
            return;
#endif
        }
        else if (type == "onnx")
        {
            factory->registerModel(name, version,
                std::make_shared<ResNet50Engine>(path, labelsPath, batchSize), type, path);
        }
        else
        {
            json err;
            err["status"] = "error";
            err["message"] = "unsupported model type: " + type;
            std::string errBody = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
            resp->setContentType("application/json");
            resp->setContentLength(errBody.size());
            resp->setBody(errBody);
            resp->setCloseConnection(false);
            return;
        }

        // Persist to config
        server_->saveConfig();

        json result;
        result["status"] = "ok";
        result["message"] = "model loaded: " + name + ":" + version;
        std::string resultBody = result.dump();
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setContentType("application/json");
        resp->setContentLength(resultBody.size());
        resp->setBody(resultBody);
        resp->setCloseConnection(false);

        LOG_INFO << "Model loaded: " << name << ":" << version << " type=" << type;
    }
    catch (const json::exception& e)
    {
        LOG_ERROR << "ModelLoadHandler JSON parse error: " << e.what();
        json err;
        err["status"] = "error";
        err["message"] = std::string("invalid JSON: ") + e.what();
        std::string errBody = err.dump();
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
        resp->setContentType("application/json");
        resp->setContentLength(errBody.size());
        resp->setBody(errBody);
        resp->setCloseConnection(false);
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "ModelLoadHandler error: " << e.what();
        json err;
        err["status"] = "error";
        err["message"] = e.what();
        std::string errBody = err.dump();
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k500InternalServerError, "Internal Server Error");
        resp->setContentType("application/json");
        resp->setContentLength(errBody.size());
        resp->setBody(errBody);
        resp->setCloseConnection(true);
    }
}
