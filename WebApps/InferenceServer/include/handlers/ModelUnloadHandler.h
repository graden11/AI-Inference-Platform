#pragma once

#include "../../../../HttpServer/include/router/RouterHandler.h"

class InferenceServer;

class ModelUnloadHandler : public http::router::RouterHandler
{
public:
    explicit ModelUnloadHandler(InferenceServer* server) : server_(server) {}

    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override;

private:
    InferenceServer* server_;
};
