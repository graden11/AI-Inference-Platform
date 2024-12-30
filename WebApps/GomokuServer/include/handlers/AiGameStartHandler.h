#pragma once
#include "../../../../HttpServer/include/router/RouterHandler.h"
#include "../GomokuServer.h"

class AiGameStartHandler : public RouterHandler
{
public:
    explicit AiGameStartHandler(GomokuServer* server) : server_(server) {}

    void handle(const HttpRequest& req, HttpResponse* resp) override;

private:
    GomokuServer* server_;
};