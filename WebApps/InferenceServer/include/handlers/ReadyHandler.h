#pragma once
#include "../../../../HttpServer/include/router/RouterHandler.h"
#include "../InferenceServer.h"

class ReadyHandler : public http::router::RouterHandler
{
public:
    explicit ReadyHandler(InferenceServer* server) : server_(server) {}

    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override;

private:
    InferenceServer* server_;
};
