#include "../../include/handlers/ModelListHandler.h"
#include "../../include/InferenceServer.h"
#include "../../include/ModelFactory.h"

#include <muduo/base/Logging.h>

void ModelListHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    try
    {
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

        auto models = factory->listModels();
        json result = json::array();
        for (auto& m : models)
        {
            json entry;
            entry["name"] = m.name;
            entry["version"] = m.version;
            entry["type"] = m.type;
            entry["path"] = m.path;
            entry["is_latest"] = m.is_latest;
            result.push_back(entry);
        }

        std::string body = result.dump(2);
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setContentType("application/json");
        resp->setContentLength(body.size());
        resp->setBody(body);
        resp->setCloseConnection(false);
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "ModelListHandler error: " << e.what();
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
