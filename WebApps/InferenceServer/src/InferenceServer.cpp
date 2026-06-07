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
#include "../include/handlers/RawPredictHandler.h"
#ifdef ENABLE_TENSORRT
#include "../include/handlers/ConvertHandler.h"
#endif
#include "../include/InferenceServer.h"
#include "../include/RequestBatcher.h"
#include "../include/ModelPipeline.h"
#include "../include/BackendRegistry.h"
#include "../include/Preprocessor.h"
#include "../include/Postprocessor.h"
#include "../../../HttpServer/include/middleware/MetricsMiddleware.h"
#include "../../../HttpServer/include/utils/MetricsCollector.h"
#include "../../../HttpServer/include/session/SessionStorage.h"
#ifdef ENABLE_REDIS
#include "../../../HttpServer/include/session/RedisSessionStorage.h"
#endif
#include <filesystem>
#include <fstream>
#include <spdlog/spdlog.h>
#include <unordered_set>
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
    // 每30秒清理一次过期的在线用户记录
    getLoop()->runEvery(30.0, [this]() {
        cleanupStaleSessions();
    });

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

#ifdef ENABLE_TENSORRT
    conversionManager_ = std::make_unique<inference::ConversionManager>();
#endif

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

    auto loadModel = [&](const std::string& name, const std::string& version,
                           const std::string& type, const std::string& path,
                           const std::string& taskStr = "classification",
                           const std::string& perModelLabels = "",
                           int topK = 5,
                           int inputW = 224, int inputH = 224, int inputC = 3,
                           const std::string& inputName = "input",
                           const std::string& outputName = "output",
                           const std::vector<float>& inputMean = {0.485f, 0.456f, 0.406f},
                           const std::vector<float>& inputStd = {0.229f, 0.224f, 0.225f}, const std::string& layout = "chw", const std::string& outputLayout = "chw") {

        if (!inference::BackendRegistry::instance().has(type)) {
            spdlog::warn("Backend '{}' not available, skipping: {}", type, name);
            return;
        }

        // Build ModelConfig from all parameters
        inference::ModelConfig cfg;
        cfg.name    = name;
        cfg.version = version.empty() ? "1" : version;
        cfg.type    = type;
        cfg.path    = path;
        cfg.task    = inference::parseTaskType(taskStr);
        // feature_extraction needs no labels; other tasks fall back to global if empty
        std::string effectiveLabels = perModelLabels;
        if (effectiveLabels.empty() && cfg.task != inference::TaskType::FEATURE_EXTRACTION)
            effectiveLabels = labelsPath;
        cfg.labels_path = effectiveLabels;
        cfg.top_k   = topK;
        cfg.max_batch_size = batchSize;
        cfg.input.name   = inputName;
        cfg.input.preferred_width  = inputW;
        cfg.input.preferred_height = inputH;
        cfg.input.channels         = inputC;
        cfg.input.layout = layout;
        cfg.input.mean = inputMean;
        cfg.input.std  = inputStd;
        cfg.output.name   = outputName;
        cfg.output.layout = outputLayout;

        if (!std::ifstream(path).good() && type == "onnx") {
            spdlog::warn("ONNX model not found, skipping: {}:{}", name, version);
            return;
        }

        auto backend = inference::BackendRegistry::instance().create(type, cfg);
        if (!backend) {
            spdlog::warn("Failed to create backend '{}' for: {}", type, name);
            return;
        }

        auto preprocessor  = inference::createPreprocessor(cfg);
        auto postprocessor = inference::createPostprocessor(cfg);

        auto pipeline = std::make_shared<inference::ModelPipeline>(
            std::move(cfg), std::move(preprocessor),
            std::move(backend), std::move(postprocessor));

        modelFactory_->registerModel(name, version, pipeline, type, path);
    };

    // Load static models from config
    for (auto &[name, entry] : config_.models) {
        std::string version = entry.version.empty() ? "1" : entry.version;
        loadModel(name, version, entry.type, entry.path,
                  entry.task, entry.labels, entry.top_k,
                  entry.input_width, entry.input_height, entry.input_channels,
                  entry.input_name, entry.output_name,
                  entry.input_mean, entry.input_std,
                  entry.input_layout, entry.output_layout);
    }

    // Load dynamic models from config (persisted by /models/load API)
    for (auto& entry : config_.dynamic_engines) {
        loadModel(entry.name, entry.version, entry.type, entry.path,
                  entry.task, entry.labels, entry.top_k,
                  entry.input_width, entry.input_height, entry.input_channels,
                  entry.input_name, entry.output_name,
                  entry.input_mean, entry.input_std,
                  entry.input_layout, entry.output_layout);
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
        auto redisStorage = std::make_unique<http::session::RedisSessionStorage>(
            config_.redis.host, config_.redis.port);
        if (redisStorage->isAvailable()) {
            storage = std::move(redisStorage);
        } else {
            spdlog::warn("Redis unavailable at {}:{}, falling back to in-memory sessions",
                         config_.redis.host, config_.redis.port);
            config_.redis.host.clear();
            storage = std::make_unique<http::session::MemorySessionStorage>();
        }
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

    // Per-IP rate limiting: 100 req/s, 200 burst
    httpServer_.enableRateLimit(100, 200);
    spdlog::info("Rate limiter enabled: 100 req/s per IP, burst 200");
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
        if (!ensureAuthenticated(req, resp)) return;
        getBackendData(req, resp);
    });

    httpServer_.Post("/predict", std::make_shared<PredictHandler>(modelFactory_.get(), batcher_.get()));
    httpServer_.Post("/predict/raw", std::make_shared<RawPredictHandler>(modelFactory_.get(), batcher_.get()));
    httpServer_.Post("/predict/batch", std::make_shared<BatchPredictHandler>(modelFactory_.get()));
    httpServer_.Post("/predict/proto", std::make_shared<ProtoPredictHandler>(modelFactory_.get()));
    httpServer_.Get("/metrics", std::make_shared<MetricsHandler>());
    httpServer_.Get("/metrics/json", std::make_shared<MetricsHandler>());
    httpServer_.Get("/health", std::make_shared<HealthHandler>());
    httpServer_.Get("/ready", std::make_shared<ReadyHandler>(this));

    // 动态模型管理
    httpServer_.Get("/models/available", [this](const http::HttpRequest& req, http::HttpResponse* resp) {
        try {
            if (!ensureAuthenticated(req, resp)) return;

            namespace fs = std::filesystem;
            std::string modelDir = fs::path(config_.labels_path).parent_path().string();

            // Build an extension → type map from the BackendRegistry
            std::vector<std::string> extFilters;
            auto backendTypes = inference::BackendRegistry::instance().list();
            for (auto& bt : backendTypes) {
                if (bt == "onnx")      extFilters.push_back(".onnx");
                if (bt == "tensorrt")  extFilters.push_back(".engine");
            }

            json files = json::array();
            for (const auto& entry : fs::directory_iterator(modelDir)) {
                if (!entry.is_regular_file()) continue;
                auto ext = entry.path().extension().string();

                bool matched = false;
                for (auto& filter : extFilters)
                    if (ext == filter) { matched = true; break; }
                if (!matched) continue;

                std::string filename = entry.path().filename().string();
                std::string stem = entry.path().stem().string();
                std::string suffix = (ext == ".onnx") ? "_onnx" : "_trt";

                json f;
                f["name"] = stem + suffix;
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

    // 列出 models 目录下所有标签文件
    httpServer_.Get("/models/labels", [this](const http::HttpRequest& req, http::HttpResponse* resp) {
        try {
            if (!ensureAuthenticated(req, resp)) return;

            namespace fs = std::filesystem;
            std::string modelDir = fs::path(config_.labels_path).parent_path().string();
            json files = json::array();
            for (const auto& entry : fs::directory_iterator(modelDir)) {
                if (!entry.is_regular_file()) continue;
                if (entry.path().extension() == ".txt") {
                    json f;
                    f["filename"] = entry.path().filename().string();
                    f["path"] = modelDir + "/" + entry.path().filename().string();
                    files.push_back(f);
                }
            }
            std::string body = files.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
            resp->setContentType("application/json");
            resp->setContentLength(body.size());
            resp->setBody(body);
            resp->setCloseConnection(false);
        } catch (const std::exception& e) {
            json err; err["status"]="error"; err["message"]=e.what();
            std::string b = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k500InternalServerError, "Internal Server Error");
            resp->setContentType("application/json"); resp->setContentLength(b.size()); resp->setBody(b);
            resp->setCloseConnection(true);
        }
    });

    // 删除模型文件
    namespace fs = std::filesystem;
    httpServer_.addRoute(http::HttpRequest::kPost, "/models/delete", [this](const http::HttpRequest& req, http::HttpResponse* resp) {
        try {
            if (!ensureAuthenticated(req, resp)) return;

            json reqBody = json::parse(req.getBody());
            std::string filePath = reqBody.value("path", "");
            if (filePath.empty()) {
                json err; err["status"]="error"; err["message"]="path is required";
                std::string b = err.dump();
                resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
                resp->setContentType("application/json"); resp->setContentLength(b.size()); resp->setBody(b);
                resp->setCloseConnection(false); return;
            }
            // 防止路径穿越：只允许删除 models 目录下的文件
            std::string modelDir = fs::path(config_.labels_path).parent_path().string();
            fs::path modelRoot = fs::weakly_canonical(modelDir);
            fs::path targetPath = fs::weakly_canonical(filePath);
            fs::path relativeTarget = fs::relative(targetPath, modelRoot);
            if (relativeTarget.empty() || relativeTarget.begin()->string() == ".." || relativeTarget.is_absolute()) {
                json err; err["status"]="error"; err["message"]="invalid path";
                std::string b = err.dump();
                resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
                resp->setContentType("application/json"); resp->setContentLength(b.size()); resp->setBody(b);
                resp->setCloseConnection(false); return;
            }
            // 检查文件是否正在被加载（在删除前检查，避免竞态）
            auto loaded = modelFactory_->listModels();
            std::string reqPathNorm = targetPath.string();
            for (auto& m : loaded) {
                std::string mPathNorm = fs::weakly_canonical(m.path).string();
                if (reqPathNorm == mPathNorm) {
                    json err; err["status"]="error"; err["message"]="model is currently loaded, unload first";
                    std::string b = err.dump();
                    resp->setStatusLine(req.getVersion(), http::HttpResponse::k409Conflict, "Conflict");
                    resp->setContentType("application/json"); resp->setContentLength(b.size()); resp->setBody(b);
                    resp->setCloseConnection(false); return;
                }
            }
            std::error_code ec;
            if (!fs::remove(targetPath, ec)) {
                json err; err["status"]="error"; err["message"]="file not found: "+filePath;
                std::string b = err.dump();
                resp->setStatusLine(req.getVersion(), http::HttpResponse::k404NotFound, "Not Found");
                resp->setContentType("application/json"); resp->setContentLength(b.size()); resp->setBody(b);
                resp->setCloseConnection(false); return;
            }
            json ok; ok["status"]="ok"; ok["message"]="deleted: "+filePath;
            std::string b = ok.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
            resp->setContentType("application/json"); resp->setContentLength(b.size()); resp->setBody(b);
            resp->setCloseConnection(false);
            LOG_INFO << "Model file deleted: " << filePath;
        } catch (const json::exception& e) {
            json err; err["status"]="error"; err["message"]=std::string("invalid JSON: ")+e.what();
            std::string b = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
            resp->setContentType("application/json"); resp->setContentLength(b.size()); resp->setBody(b);
            resp->setCloseConnection(false);
        } catch (const std::exception& e) {
            json err; err["status"]="error"; err["message"]=e.what();
            std::string b = err.dump();
            resp->setStatusLine(req.getVersion(), http::HttpResponse::k500InternalServerError, "Internal Server Error");
            resp->setContentType("application/json"); resp->setContentLength(b.size()); resp->setBody(b);
            resp->setCloseConnection(true);
        }
    });
#ifdef ENABLE_TENSORRT
    httpServer_.Get("/models/convert/status", std::make_shared<ConvertHandler>(this));
    httpServer_.Post("/models/convert", std::make_shared<ConvertHandler>(this));
#endif
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

bool InferenceServer::ensureAuthenticated(const http::HttpRequest& req, http::HttpResponse* resp) const
{
    auto* manager = getSessionManager();
    auto session = manager ? manager->getSession(req, resp) : nullptr;
    if (session && session->getValue("isLoggedIn") == "true")
        return true;

    json err;
    err["status"] = "error";
    err["message"] = "Unauthorized";
    std::string body = err.dump();
    resp->setStatusLine(req.getVersion(), http::HttpResponse::k401Unauthorized, "Unauthorized");
    resp->setContentType("application/json");
    resp->setContentLength(body.size());
    resp->setBody(body);
    resp->setCloseConnection(true);
    return false;
}

void InferenceServer::cleanupStaleSessions()
{
    auto* sm = getSessionManager();
    if (!sm) return;

    std::vector<int> staleUsers;

    // Always lock onlineUsers_ first, then loginSessions_ (avoids ABBA deadlock with LoginHandler)
    auto sessionIds = sm->getActiveSessionIds();
    std::unordered_set<std::string> active(sessionIds.begin(), sessionIds.end());

    {
        std::lock_guard<std::mutex> lock1(mutexForOnlineUsers_);
        std::lock_guard<std::mutex> lock2(mutexForLoginSessions_);
        for (auto it = loginSessions_.begin(); it != loginSessions_.end(); ) {
            if (active.find(it->second) == active.end()) {
                int uid = it->first;
                LOG_INFO << "Cleaning stale session: userId=" << uid;
                it = loginSessions_.erase(it);
                staleUsers.push_back(uid);
            } else {
                ++it;
            }
        }
        for (int uid : staleUsers) {
            onlineUsers_.erase(uid);
        }
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
                entry["task"] = m.task;
                entry["labels"] = m.labels;
                entry["top_k"] = m.top_k;
                entry["input_name"]     = m.input_name;
                entry["output_name"]    = m.output_name;
                entry["input_width"]    = m.input_width;
                entry["input_height"]   = m.input_height;
                entry["input_channels"] = m.input_channels;
                entry["input_layout"]   = m.input_layout;
                entry["output_layout"]  = m.output_layout;
                entry["input_mean"]     = m.input_mean;
                entry["input_std"]      = m.input_std;
                entry["confidence_threshold"] = m.confidence_threshold;
                entry["nms_threshold"]  = m.nms_threshold;
                entry["max_detections"] = m.max_detections;
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

