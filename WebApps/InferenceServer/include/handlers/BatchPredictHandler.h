#pragma once

#include "../../../../HttpServer/include/router/RouterHandler.h"

class ModelFactory;

class BatchPredictHandler : public http::router::RouterHandler
{
public:
    explicit BatchPredictHandler(ModelFactory* factory) : factory_(factory) {}

    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override;

private:
    ModelFactory* factory_;
};
