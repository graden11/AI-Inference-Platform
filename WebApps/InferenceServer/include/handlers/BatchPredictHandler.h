#pragma once

#include "../../../../HttpServer/include/router/RouterHandler.h"

class ModelFactory;
class RequestBatcher;
class RequestSlotPool;

class BatchPredictHandler : public http::router::RouterHandler
{
public:
    explicit BatchPredictHandler(ModelFactory* factory,
                                 RequestBatcher* batcher,
                                 RequestSlotPool* slotPool)
        : factory_(factory), batcher_(batcher), slotPool_(slotPool) {}

    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override;

private:
    ModelFactory* factory_;
    RequestBatcher* batcher_;
    RequestSlotPool* slotPool_;
};
