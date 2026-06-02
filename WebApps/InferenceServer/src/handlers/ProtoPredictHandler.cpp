#include "../../include/handlers/ProtoPredictHandler.h"
#include "../../include/ModelFactory.h"
#include "../../include/InferenceEngine.h"

#include <inference.pb.h>

#include <muduo/base/Logging.h>

#include "../../../../HttpServer/include/utils/JsonUtil.h"

void ProtoPredictHandler::handle(const http::HttpRequest &req, http::HttpResponse *resp)
{
    inference::PredictRequest request;
    if (!request.ParseFromString(req.getBody()))
    {
        inference::PredictResponse response;
        response.set_success(false);
        response.set_message("failed to parse PredictRequest protobuf");
        std::string out;
        response.SerializeToString(&out);
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
        resp->setContentType("application/x-protobuf");
        resp->setContentLength(out.size());
        resp->setBody(out);
        resp->setCloseConnection(false);
        return;
    }

    std::string modelName = request.model_name().empty() ? "resnet50" : request.model_name();
    auto engine = factory_->getModel(modelName);
    if (!engine)
    {
        inference::PredictResponse response;
        response.set_success(false);
        response.set_message("unknown model: " + modelName);
        std::string out;
        response.SerializeToString(&out);
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
        resp->setContentType("application/x-protobuf");
        resp->setContentLength(out.size());
        resp->setBody(out);
        resp->setCloseConnection(false);
        return;
    }

    std::vector<uint8_t> imageData(request.image_data().begin(),
                                   request.image_data().end());
    std::string resultJson = engine->predictFromBytes(imageData);

    inference::PredictResponse response;

    try
    {
        json result = json::parse(resultJson);
        if (result["status"] == "ok")
        {
            response.set_success(true);
            for (auto &p : result["predictions"])
            {
                auto *pred = response.add_predictions();
                pred->set_class_id(p["id"].get<int>());
                pred->set_label(p["label"].get<std::string>());
                pred->set_confidence(p["confidence"].get<float>() / 100.0f);
            }
        }
        else
        {
            response.set_success(false);
            response.set_message(result.value("message", "unknown error"));
        }
    }
    catch (const json::exception &e)
    {
        response.set_success(false);
        response.set_message(std::string("JSON parse error: ") + e.what());
    }

    std::string out;
    response.SerializeToString(&out);
    resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
    resp->setContentType("application/x-protobuf");
    resp->setContentLength(out.size());
    resp->setBody(out);
    resp->setCloseConnection(false);
}
