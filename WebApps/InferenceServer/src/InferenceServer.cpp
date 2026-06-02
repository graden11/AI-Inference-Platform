#include "../include/handlers/EntryHandler.h"
#include "../include/handlers/LoginHandler.h"
#include "../include/handlers/RegisterHandler.h"
#include "../include/handlers/MenuHandler.h"
#include "../include/handlers/LogoutHandler.h"
#include "../include/handlers/GameBackendHandler.h"
#include "../include/handlers/PredictHandler.h"
#include "../include/handlers/BatchPredictHandler.h"
#include "../include/handlers/ProtoPredictHandler.h"
#include "../include/handlers/MetricsHandler.h"
#include "../include/handlers/ModelLoadHandler.h"
#include "../include/handlers/ModelListHandler.h"
#include "../include/handlers/ModelUnloadHandler.h"
#include "../include/handlers/HealthHandler.h"
#include "../include/handlers/ReadyHandler.h"
#include "../include/InferenceServer.h"
#include "../include/RequestBatcher.h"
#include "../include/ResNet50Engine.h"
#ifdef ENABLE_TENSORRT
#include "../include/ResNet50TRTEngine.h"
#endif
#include "../../../HttpServer/include/middleware/MetricsMiddleware.h"
#include "../../../HttpServer/include/utils/MetricsCollector.h"
#include "../../../HttpServer/include/session/SessionStorage.h"
#ifdef ENABLE_REDIS
#include "../../../HttpServer/include/session/RedisSessionStorage.h"
#endif
#include <filesystem>
#include <fstream>
#include <spdlog/spdlog.h>
#include "../../../HttpServer/include/http/HttpRequest.h"
#include "../../../HttpServer/include/http/HttpResponse.h"
#include "../../../HttpServer/include/http/HttpServer.h"

using namespace http;

InferenceServer::InferenceServer(const AppConfig &cfg,
                           muduo::net::TcpServer::Option option)
    : httpServer_(cfg.server.port, cfg.server.name, option), maxOnline_(0), config_(cfg)
{
    initialize();
}

void InferenceServer::setThreadNum(int numThreads)
{
    httpServer_.setThreadNum(numThreads);
}

void InferenceServer::start()
{
    httpServer_.start();
    cleanup();
}

void InferenceServer::cleanup()
{
    LOG_INFO << "Cleaning up resources...";
    if (batcher_)
    {
        batcher_->stop();
        batcher_.reset();
    }
    modelFactory_.reset();
    LOG_INFO << "Cleanup complete";
}

void InferenceServer::initialize()
{
    http::MysqlUtil::init(config_.mysql.host,
                          config_.mysql.user,
                          config_.mysql.password,
                          config_.mysql.database,
                          config_.mysql.pool_size);
    if (config_.mysql.user.empty() || config_.mysql.password.empty())
    {
        spdlog::warn("MySQL credentials are empty — set MYSQL_USER / MYSQL_PASSWORD env vars or edit config.json");
    }
    initializeSession();
    initializeMiddleware();
    MetricsCollector::instance().setInflightSource(httpServer_.getInflightPtr());
    // 必须在initializeRouter之前，因为路由注册会用到modelFactory_
    modelFactory_ = std::make_unique<ModelFactory>();

    // 初始化批处理器（在模型加载之前创建，因为路由注册需要它）
    if (config_.batching.enabled)
    {
        batcher_ = std::make_shared<RequestBatcher>(
            modelFactory_.get(),
            config_.batching.max_batch_size,
            std::chrono::milliseconds(config_.batching.max_delay_ms));
        batcher_->start();
        LOG_INFO << "Dynamic batching enabled: maxBatchSize=" << config_.batching.max_batch_size
                 << ", maxDelayMs=" << config_.batching.max_delay_ms;
    }

    const std::string &labelsPath = config_.labels_path;

    int batchSize = config_.batching.enabled ? config_.batching.max_batch_size : 1;

    auto loadEngine = [&](const std::string& name, const std::string& version,
                           const std::string& type, const std::string& path) {
        if (type == "tensorrt") {
#ifdef ENABLE_TENSORRT
            if (!std::ifstream(path).good()) {
                spdlog::warn("TensorRT engine not found, skipping: {}:{}", name, version);
                return;
            }
            modelFactory_->registerModel(name, version,
                std::make_shared<ResNet50TRTEngine>(path, labelsPath, batchSize), type, path);
#else
            spdlog::warn("TensorRT support disabled, skipping: {}", name);
#endif
        } else if (type == "onnx") {
            if (!std::ifstream(path).good()) {
                spdlog::warn("ONNX model not found, skipping: {}:{}", name, version);
                return;
            }
            modelFactory_->registerModel(name, version,
                std::make_shared<ResNet50Engine>(path, labelsPath, batchSize), type, path);
        }
    };

    // Load static models from config
    for (auto &[name, entry] : config_.models) {
        std::string version = entry.version.empty() ? "1" : entry.version;
        loadEngine(name, version, entry.type, entry.path);
    }

    // Load dynamic models from config (persisted by /models/load API)
    for (auto& entry : config_.dynamic_engines) {
        loadEngine(entry.name, entry.version, entry.type, entry.path);
    }

    initializeRouter();
}

void InferenceServer::initializeSession()
{
    std::unique_ptr<http::session::SessionStorage> storage;
#ifdef ENABLE_REDIS
    if (config_.redis.host.empty()) {
        storage = std::make_unique<http::session::MemorySessionStorage>();
    } else {
        storage = std::make_unique<http::session::RedisSessionStorage>(
            config_.redis.host, config_.redis.port);
    }
#else
    storage = std::make_unique<http::session::MemorySessionStorage>();
#endif
    auto sessionManager = std::make_unique<http::session::SessionManager>(std::move(storage));
    setSessionManager(std::move(sessionManager));
}

void InferenceServer::initializeMiddleware()
{
    auto corsMiddleware = std::make_shared<http::middleware::CorsMiddleware>();
    auto metricsMiddleware = std::make_shared<http::middleware::MetricsMiddleware>();
    httpServer_.addMiddleware(metricsMiddleware);
    httpServer_.addMiddleware(corsMiddleware);
}

void InferenceServer::initializeRouter()
{
    httpServer_.Get("/", std::make_shared<EntryHandler>(this));
    httpServer_.Get("/entry", std::make_shared<EntryHandler>(this));
    httpServer_.Post("/login", std::make_shared<LoginHandler>(this));
    httpServer_.Post("/register", std::make_shared<RegisterHandler>(this));
    httpServer_.Post("/user/logout", std::make_shared<LogoutHandler>(this));
    httpServer_.Get("/menu", std::make_shared<MenuHandler>(this));

    httpServer_.Get("/backend", std::make_shared<GameBackendHandler>(this));
    httpServer_.Get("/backend_data", [this](const http::HttpRequest& req, http::HttpResponse* resp) {
        getBackendData(req, resp);
    });

    httpServer_.Post("/predict", std::make_shared<PredictHandler>(modelFactory_.get(), batcher_.get()));
    httpServer_.Post("/predict/batch", std::make_shared<BatchPredictHandler>(modelFactory_.get()));
    httpServer_.Post("/predict/proto", std::make_shared<ProtoPredictHandler>(modelFactory_.get()));
    httpServer_.Get("/metrics", std::make_shared<MetricsHandler>());
    httpServer_.Get("/metrics/json", std::make_shared<MetricsHandler>());
    httpServer_.Get("/health", std::make_shared<HealthHandler>());
    httpServer_.Get("/ready", std::make_shared<ReadyHandler>(this));

    // 动态模型管理
    httpServer_.Get("/models/available", [this](const http::HttpRequest& req, http::HttpResponse* resp) {
        try {
            namespace fs = std::filesystem;
            std::string modelDir = fs::path(config_.labels_path).parent_path().string();

            json files = json::array();
            for (const auto& entry : fs::directory_iterator(modelDir)) {
                if (!entry.is_regular_file()) continue;
                auto ext = entry.path().extension().string();
                if (ext != ".onnx" && ext != ".engine") continue;

                std::string filename = entry.path().filename().string();
                std::string stem = entry.path().stem().string();

                json f;
                f["name"] = stem;
                f["filename"] = filename;
                f["path"] = modelDir + "/" + filename;
                f["type"] = (ext == ".onnx") ? "onnx" : "tensorrt";
                files.push_back(f);
            }

            std::string body = files.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
            resp->setContentType("application/json");
            resp->setContentLength(body.size());
            resp->setBody(body);
            resp->setCloseConnection(false);
        } catch (const std::exception& e) {
            json err;
            err["status"] = "error";
            err["message"] = e.what();
            std::string body = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k500InternalServerError,
                               "Internal Server Error");
            resp->setContentType("application/json");
            resp->setContentLength(body.size());
            resp->setBody(body);
            resp->setCloseConnection(true);
        }
    });
    httpServer_.Post("/models/load", std::make_shared<ModelLoadHandler>(this));
    httpServer_.Get("/models", std::make_shared<ModelListHandler>(this));
    httpServer_.addRoute(http::HttpRequest::kDelete, "/models/:name/:version",
                         std::make_shared<ModelUnloadHandler>(this));
}

// 获取后台数据
void InferenceServer::getBackendData(const http::HttpRequest &req, http::HttpResponse *resp)
{
    try 
    {
        // 获取数据
        int curOnline = getCurOnline();
        LOG_INFO << "当前在线人数: " << curOnline;
        
        int maxOnline = getMaxOnline();
        LOG_INFO << "历史最高在线人数: " << maxOnline;
        
        int totalUser = getUserCount();
        LOG_INFO << "已注册用户总数: " << totalUser;

        // 构造 JSON 响应
        nlohmann::json respBody;
        respBody = {
            {"curOnline", curOnline},
            {"maxOnline", maxOnline},
            {"totalUser", totalUser}
        };

        // 转换为字符串
        std::string responseStr = respBody.dump(4);
        
        // 设置响应
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
        resp->setContentType("application/json");
        resp->setBody(responseStr);
        resp->setContentLength(responseStr.size());
        resp->setCloseConnection(false);

        LOG_INFO << "Backend data response prepared successfully";
    }
    catch (const std::exception& e) 
    {
        LOG_ERROR << "Error in getBackendData: " << e.what();
        
        // 错误响应
        nlohmann::json errorBody = {
            {"error", "Internal Server Error"},
            {"message", e.what()}
        };
        
        std::string errorStr = errorBody.dump();
        resp->setStatusCode(http::HttpResponse::k500InternalServerError);
        resp->setStatusMessage("Internal Server Error");
        resp->setContentType("application/json");
        resp->setBody(errorStr);
        resp->setContentLength(errorStr.size());
        resp->setCloseConnection(true);
    }
}

void InferenceServer::saveConfig() const
{
    try
    {
        json j;
        std::ifstream f(configPath_);
        if (f.good())
        {
            f >> j;
        }

        json dynamicEngines = json::array();
        auto models = modelFactory_->listModels();
        for (auto& m : models)
        {
            // Only save dynamic models (not those from static config)
            bool isStatic = false;
            for (auto& [name, entry] : config_.models)
            {
                std::string cfgVersion = entry.version.empty() ? "1" : entry.version;
                if (name == m.name && cfgVersion == m.version)
                {
                    isStatic = true;
                    break;
                }
            }
            if (!isStatic)
            {
                json entry;
                entry["name"] = m.name;
                entry["version"] = m.version;
                entry["type"] = m.type;
                entry["path"] = m.path;
                dynamicEngines.push_back(entry);
            }
        }
        j["dynamic_engines"] = dynamicEngines;

        std::ofstream of(configPath_);
        of << j.dump(2) << std::endl;
        LOG_INFO << "Config saved to " << configPath_;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "Failed to save config: " << e.what();
    }
}

void InferenceServer::packageResp(const std::string &version,
                             http::HttpResponse::HttpStatusCode statusCode,
                             const std::string &statusMsg,
                             bool close,
                             const std::string &contentType,
                             int contentLen,
                             const std::string &body,
                             http::HttpResponse *resp)
{
    if (resp == nullptr) 
    {
        LOG_ERROR << "Response pointer is null";
        return;
    }

    try 
    {
        resp->setVersion(version);
        resp->setStatusCode(statusCode);
        resp->setStatusMessage(statusMsg);
        resp->setCloseConnection(close);
        resp->setContentType(contentType);
        resp->setContentLength(contentLen);
        resp->setBody(body);
        
        LOG_INFO << "Response packaged successfully";
    }
    catch (const std::exception& e) 
    {
        LOG_ERROR << "Error in packageResp: " << e.what();
        // 设置一个基本的错误响应
        resp->setStatusCode(http::HttpResponse::k500InternalServerError);
        resp->setStatusMessage("Internal Server Error");
        resp->setCloseConnection(true);
    }
}

