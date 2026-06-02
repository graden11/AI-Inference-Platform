#include "../../include/handlers/HealthHandler.h"

void HealthHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    std::string body = R"({"status":"ok"})";
    resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
    resp->setContentType("application/json");
    resp->setContentLength(body.size());
    resp->setBody(body);
    resp->setCloseConnection(false);
}
