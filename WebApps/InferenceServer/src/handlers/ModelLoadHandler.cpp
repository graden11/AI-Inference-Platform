#include "../../include/handlers/ModelLoadHandler.h"
#include "../../include/InferenceServer.h"
#include "../../include/ModelFactory.h"
#include "../../include/ModelPipeline.h"
#include "../../include/BackendRegistry.h"
#include "../../include/Preprocessor.h"
#include "../../include/Postprocessor.h"

#include <muduo/base/Logging.h>
#include <fstream>

void ModelLoadHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    try
    {
        auto session = server_->getSessionManager()->getSession(req, resp);
        if (session->getValue("isLoggedIn") != "true")
        {
            json err;
            err["status"] = "error";
            err["message"] = "Unauthorized";
            std::string errBody = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k401Unauthorized, "Unauthorized");
            resp->setContentType("application/json");
            resp->setContentLength(errBody.size());
            resp->setBody(errBody);
            resp->setCloseConnection(true);
            return;
        }

        json body = json::parse(req.getBody());

        std::string name = body.value("name", "");
        std::string version = body.value("version", "1");
        std::string type = body.value("type", "onnx");
        std::string path = body.value("path", "");

        if (name.empty() || path.empty())
        {
            json err;
            err["status"] = "error";
            err["message"] = "name and path are required";
            std::string errBody = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
            resp->setContentType("application/json");
            resp->setContentLength(errBody.size());
            resp->setBody(errBody);
            resp->setCloseConnection(false);
            return;
        }

        auto* factory = server_->getModelFactory();
        if (!factory)
        {
            json err;
            err["status"] = "error";
            err["message"] = "ModelFactory not initialized";
            std::string errBody = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k500InternalServerError, "Internal Server Error");
            resp->setContentType("application/json");
            resp->setContentLength(errBody.size());
            resp->setBody(errBody);
            resp->setCloseConnection(true);
            return;
        }

        // Check if already loaded
        if (factory->hasModel(name, version))
        {
            json err;
            err["status"] = "error";
            err["message"] = "model already loaded: " + name + ":" + version;
            std::string errBody = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k409Conflict, "Conflict");
            resp->setContentType("application/json");
            resp->setContentLength(errBody.size());
            resp->setBody(errBody);
            resp->setCloseConnection(false);
            return;
        }

        // Check file exists
        if (!std::ifstream(path).good())
        {
            json err;
            err["status"] = "error";
            err["message"] = "model file not found: " + path;
            std::string errBody = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
            resp->setContentType("application/json");
            resp->setContentLength(errBody.size());
            resp->setBody(errBody);
            resp->setCloseConnection(false);
            return;
        }

        const std::string& labelsPath = server_->getLabelsPath();
        int batchSize = server_->config_.batching.enabled ? server_->config_.batching.max_batch_size : 1;

        // Parse optional extended fields from request
        std::string taskStr   = body.value("task", "classification");
        std::string perModelLabels = body.value("labels", "");
        int topK   = body.value("top_k", 5);
        int inputW = body.value("input_width", 224);
        int inputH = body.value("input_height", 224);
        int inputC = body.value("input_channels", 3);
        std::string inputName  = body.value("input_name", "input");
        std::string outputName = body.value("output_name", "output");
        std::string layout     = body.value("layout", "chw");
        std::string outLayout  = body.value("output_layout", "chw");
        std::vector<float> inputMean = {0.485f, 0.456f, 0.406f};
        std::vector<float> inputStd  = {0.229f, 0.224f, 0.225f};

        if (body.contains("input_mean")) {
            inputMean.clear();
            for (auto& v : body["input_mean"]) inputMean.push_back(v.get<float>());
        }
        if (body.contains("input_std")) {
            inputStd.clear();
            for (auto& v : body["input_std"]) inputStd.push_back(v.get<float>());
        }

        // Check backend availability
        if (!inference::BackendRegistry::instance().has(type))
        {
            json err;
            err["status"] = "error";
            err["message"] = "unsupported model type: " + type;
            std::string errBody = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
            resp->setContentType("application/json");
            resp->setContentLength(errBody.size());
            resp->setBody(errBody);
            resp->setCloseConnection(false);
            return;
        }

        // Build ModelConfig
        std::string effectiveLabels = perModelLabels.empty() ? labelsPath : perModelLabels;
        inference::ModelConfig cfg;
        cfg.name    = name;
        cfg.version = version;
        cfg.type    = type;
        cfg.path    = path;
        cfg.task    = inference::parseTaskType(taskStr);
        cfg.labels_path = effectiveLabels;
        cfg.top_k   = topK;
        cfg.max_batch_size = batchSize;
        cfg.input.name   = inputName;
        cfg.input.preferred_width  = inputW;
        cfg.input.preferred_height = inputH;
        cfg.input.channels         = inputC;
        cfg.input.layout = layout;
        cfg.output.layout = outLayout;
        cfg.input.mean = inputMean;
        cfg.input.std  = inputStd;
        cfg.output.name = outputName;

        // Create pipeline
        auto backend = inference::BackendRegistry::instance().create(type, cfg);
        if (!backend)
        {
            json err;
            err["status"] = "error";
            err["message"] = "failed to create backend for type: " + type;
            std::string errBody = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k500InternalServerError, "Internal Server Error");
            resp->setContentType("application/json");
            resp->setContentLength(errBody.size());
            resp->setBody(errBody);
            resp->setCloseConnection(true);
            return;
        }

        auto preprocessor  = inference::createPreprocessor(cfg);
        auto postprocessor = inference::createPostprocessor(cfg);

        auto pipeline = std::make_shared<inference::ModelPipeline>(
            std::move(cfg), std::move(preprocessor),
            std::move(backend), std::move(postprocessor));

        factory->registerModel(name, version, pipeline, type, path);

        // Persist to config
        server_->saveConfig();

        json result;
        result["status"] = "ok";
        result["message"] = "model loaded: " + name + ":" + version;
        std::string resultBody = result.dump();
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setContentType("application/json");
        resp->setContentLength(resultBody.size());
        resp->setBody(resultBody);
        resp->setCloseConnection(false);

        LOG_INFO << "Model loaded: " << name << ":" << version << " type=" << type;
    }
    catch (const json::exception& e)
    {
        LOG_ERROR << "ModelLoadHandler JSON parse error: " << e.what();
        json err;
        err["status"] = "error";
        err["message"] = std::string("invalid JSON: ") + e.what();
        std::string errBody = err.dump();
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
        resp->setContentType("application/json");
        resp->setContentLength(errBody.size());
        resp->setBody(errBody);
        resp->setCloseConnection(false);
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "ModelLoadHandler error: " << e.what();
        json err;
        err["status"] = "error";
        err["message"] = e.what();
        std::string errBody = err.dump();
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k500InternalServerError, "Internal Server Error");
        resp->setContentType("application/json");
        resp->setContentLength(errBody.size());
        resp->setBody(errBody);
        resp->setCloseConnection(true);
    }
}
