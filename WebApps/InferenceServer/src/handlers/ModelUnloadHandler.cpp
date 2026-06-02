#include "../../include/handlers/ModelUnloadHandler.h"
#include "../../include/InferenceServer.h"
#include "../../include/ModelFactory.h"

#include <muduo/base/Logging.h>

void ModelUnloadHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
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

        std::string name = req.getPathParameters("param1");
        std::string version = req.getPathParameters("param2");

        if (name.empty() || version.empty())
        {
            json err;
            err["status"] = "error";
            err["message"] = "name and version are required in path";
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

        if (!factory->unloadModel(name, version))
        {
            json err;
            err["status"] = "error";
            err["message"] = "model not found: " + name + ":" + version;
            std::string errBody = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k404NotFound, "Not Found");
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
        result["message"] = "model unloaded: " + name + ":" + version;
        std::string resultBody = result.dump();
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setContentType("application/json");
        resp->setContentLength(resultBody.size());
        resp->setBody(resultBody);
        resp->setCloseConnection(false);

        LOG_INFO << "Model unloaded: " << name << ":" << version;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "ModelUnloadHandler error: " << e.what();
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
