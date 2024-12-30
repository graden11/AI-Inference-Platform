#pragma once
#include "../../../../HttpServer/include/router/RouterHandler.h"
#include "../GomokuServer.h"
#include "../../../HttpServer/include/utils/JsonUtil.h"

class LogoutHandler : public RouterHandler {
public:
    explicit LogoutHandler(GomokuServer* server) : server_(server) {}
    void handle(const HttpRequest& req, HttpResponse* resp) override;
private:
    GomokuServer* server_;
};