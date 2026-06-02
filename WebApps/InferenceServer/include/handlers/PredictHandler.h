#pragma once

#include "../../../../HttpServer/include/router/RouterHandler.h"
#include "../../../../HttpServer/include/utils/JsonUtil.h"

class ModelFactory;
class RequestBatcher;

class PredictHandler : public http::router::RouterHandler
{
public:
    explicit PredictHandler(ModelFactory* factory, RequestBatcher* batcher = nullptr)
        : factory_(factory), batcher_(batcher) {}

    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override;

private:
    ModelFactory* factory_;
    RequestBatcher* batcher_;
};
