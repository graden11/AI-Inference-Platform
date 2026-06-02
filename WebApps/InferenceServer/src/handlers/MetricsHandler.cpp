#include "../../include/handlers/MetricsHandler.h"
#include "../../../HttpServer/include/utils/MetricsCollector.h"

void MetricsHandler::handle(const http::HttpRequest &req, http::HttpResponse *resp)
{
    std::string body;
    std::string contentType;

    if (req.path() == "/metrics/json")
    {
        body = MetricsCollector::instance().toJson().dump(2);
        contentType = "application/json";
    }
    else
    {
        body = MetricsCollector::instance().toPrometheus();
        contentType = "text/plain; version=0.0.4";
    }

    resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
    resp->setContentType(contentType);
    resp->setContentLength(body.size());
    resp->setBody(body);
    resp->setCloseConnection(false);
}
