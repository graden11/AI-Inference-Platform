#pragma once

#include "../../../../HttpServer/include/router/RouterHandler.h"
#include "../../../../HttpServer/include/utils/JsonUtil.h"

class InferenceServer;

class ModelLoadHandler : public http::router::RouterHandler
{
public:
    explicit ModelLoadHandler(InferenceServer* server) : server_(server) {}

    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override;

private:
    InferenceServer* server_;
};
