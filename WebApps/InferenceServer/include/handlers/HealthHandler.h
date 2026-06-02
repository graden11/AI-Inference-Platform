#pragma once
#include "../../../../HttpServer/include/router/RouterHandler.h"

class HealthHandler : public http::router::RouterHandler
{
public:
    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override;
};
