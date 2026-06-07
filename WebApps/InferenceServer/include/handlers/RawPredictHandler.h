#pragma once

#include "../../../../HttpServer/include/router/RouterHandler.h"
#include "../../../../HttpServer/include/utils/JsonUtil.h"

class ModelFactory;
class RequestBatcher;

/// Handler for POST /predict/raw — accepts raw JPEG/PNG bytes as the body,
/// avoiding base64 encoding and JSON parsing overhead (~12ms on mobile SoCs).
///
/// Usage:
///   POST /predict/raw?model_name=squeezenet1.1-7_onnx
///   Content-Type: image/jpeg
///   <raw JPEG bytes>
///
/// Response is the same JSON format as /predict.
class RawPredictHandler : public http::router::RouterHandler
{
public:
    explicit RawPredictHandler(ModelFactory* factory, RequestBatcher* batcher = nullptr)
        : factory_(factory), batcher_(batcher) {}

    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override;

private:
    ModelFactory* factory_;
    RequestBatcher* batcher_;
};
