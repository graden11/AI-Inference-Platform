#include "../include/handlers/EntryHandler.h"
#include "../include/handlers/LoginHandler.h"
#include "../include/handlers/RegisterHandler.h"
#include "../include/handlers/MenuHandler.h"
#include "../include/handlers/LogoutHandler.h"
#include "../include/handlers/GameBackendHandler.h"
#include "../include/handlers/PredictHandler.h"
#include "../include/handlers/ProtoPredictHandler.h"
#include "../include/handlers/MetricsHandler.h"
#include "../include/InferenceServer.h"
#include "../include/ResNet50Engine.h"
#ifdef ENABLE_TENSORRT
#include "../include/ResNet50TRTEngine.h"
#endif
#include "../../../HttpServer/include/middleware/MetricsMiddleware.h"
#include "../../../HttpServer/include/session/SessionStorage.h"
#ifdef ENABLE_REDIS
#include "../../../HttpServer/include/session/RedisSessionStorage.h"
#endif
#include <fstream>
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
}

void InferenceServer::initialize()
{
    http::MysqlUtil::init(config_.mysql.host,
                          config_.mysql.user,
                          config_.mysql.password,
                          config_.mysql.database,
                          config_.mysql.pool_size);
    initializeSession();
    initializeMiddleware();
    // 必须在initializeRouter之前，因为路由注册会用到modelFactory_
    modelFactory_ = std::make_unique<ModelFactory>();
    const std::string &labelsPath = config_.labels_path;

    for (auto &[name, entry] : config_.models)
    {
#ifdef ENABLE_TENSORRT
        if (entry.type == "tensorrt")
        {
            if (!std::ifstream(entry.path).good())
            {
                LOG_INFO << "TensorRT engine not found, skipping: " << name;
                continue;
            }
            modelFactory_->registerModel(name,
                std::make_unique<ResNet50TRTEngine>(entry.path, labelsPath));
            continue;
        }
#endif
        if (entry.type == "onnx")
        {
            if (!std::ifstream(entry.path).good())
            {
                LOG_INFO << "ONNX model not found, skipping: " << name;
                continue;
            }
            modelFactory_->registerModel(name,
                std::make_unique<ResNet50Engine>(entry.path, labelsPath));
        }
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

    httpServer_.Post("/predict", std::make_shared<PredictHandler>(modelFactory_.get()));
    httpServer_.Post("/predict/proto", std::make_shared<ProtoPredictHandler>(modelFactory_.get()));
    httpServer_.Get("/metrics", std::make_shared<MetricsHandler>());
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

