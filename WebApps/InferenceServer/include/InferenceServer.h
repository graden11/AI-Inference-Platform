#pragma once

#include <atomic>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <mutex>


#include "ModelFactory.h"
#ifdef ENABLE_TENSORRT
#include "ConversionManager.h"
#endif
#include "../../../HttpServer/include/http/HttpServer.h"
#include "../../../HttpServer/include/utils/ConfigLoader.h"
#include "../../../HttpServer/include/utils/MysqlUtil.h"
#include "../../../HttpServer/include/utils/JsonUtil.h"


class LoginHandler;
class EntryHandler;
class RegisterHandler;
class MenuHandler;
class LogoutHandler;
class GameBackendHandler;
class PredictHandler;
class ProtoPredictHandler;
class ModelLoadHandler;
class ModelListHandler;
class ModelUnloadHandler;
class ConvertHandler;
class RequestBatcher;
class ReadyHandler;


class InferenceServer
{
public:
    InferenceServer(const AppConfig &cfg,
                 muduo::net::TcpServer::Option option = muduo::net::TcpServer::kNoReusePort);

    void setThreadNum(int numThreads);
    void start();
    void cleanup();

    const std::string& getConfigPath() const { return configPath_; }
    void setConfigPath(const std::string& path) { configPath_ = path; }

    muduo::net::EventLoop* getLoop() { return httpServer_.getLoop(); }
    int getShutdownTimeoutMs() const { return config_.server.shutdown_timeout_ms; }
    void gracefulShutdown(std::chrono::milliseconds timeout) { httpServer_.gracefulShutdown(timeout); }
private:
    void initialize();
    void initializeSession();
    void initializeRouter();
    void initializeMiddleware();
    
    void setSessionManager(std::unique_ptr<http::session::SessionManager> manager)
    {
        httpServer_.setSessionManager(std::move(manager));
    }

    http::session::SessionManager*  getSessionManager() const
    {
        return httpServer_.getSessionManager();
    }

    ModelFactory* getModelFactory() const
    {
        return modelFactory_.get();
    }

#ifdef ENABLE_TENSORRT
    inference::ConversionManager* getConversionManager() const
    {
        return conversionManager_.get();
    }
#endif

    const std::string& getLabelsPath() const { return config_.labels_path; }
    void saveConfig() const;
    bool ensureAuthenticated(const http::HttpRequest& req, http::HttpResponse* resp) const;

    // 清理已过期的在线用户记录
    void cleanupStaleSessions();

    void getBackendData(const http::HttpRequest& req, http::HttpResponse* resp);

    /// GPU-side tensor endpoint — called by RemoteBackend on the cloud server.
    /// Accepts preprocessed float tensors, runs inference, returns raw output.
    void handleTensorPredict(const http::HttpRequest& req, http::HttpResponse* resp);

    void packageResp(const std::string& version, http::HttpResponse::HttpStatusCode statusCode,
                     const std::string& statusMsg, bool close, const std::string& contentType,
                     int contentLen, const std::string& body, http::HttpResponse* resp);

    // 获取历史最高在线人数
    int getMaxOnline() const
    {
        return maxOnline_.load();
    }

    // 获取当前在线人数
    int getCurOnline() const
    {
        std::lock_guard<std::mutex> lock(mutexForOnlineUsers_);
        return static_cast<int>(onlineUsers_.size());
    }

    void updateMaxOnline(int online)
    {
        maxOnline_ = std::max(maxOnline_.load(), online);
    }

    // 获取用户总数
    int getUserCount()
    {
        std::string sql = "SELECT COUNT(*) as count FROM users";

        sql::ResultSet* res = mysqlUtil_.executeQuery(sql);
        if (res->next())
        {
            return res->getInt("count");
        }
        return 0;
    }
    
private:
    friend class EntryHandler;
    friend class LoginHandler;
    friend class RegisterHandler;
    friend class MenuHandler;
    friend class LogoutHandler;
    friend class GameBackendHandler;
    friend class ProtoPredictHandler;
    friend class ModelLoadHandler;
    friend class ModelListHandler;
    friend class ModelUnloadHandler;
    friend class ConvertHandler;
    friend class ReadyHandler;

private:
    http::HttpServer                                 httpServer_;
    http::MysqlUtil                                  mysqlUtil_;
    // userId -> 是否在线
    std::unordered_map<int, bool>                    onlineUsers_;
    mutable std::mutex                               mutexForOnlineUsers_;
    // userId -> sessionId（踢人下线用）
    std::unordered_map<int, std::string>             loginSessions_;
    std::mutex                                       mutexForLoginSessions_;
    // 最高在线人数
    std::atomic<int>                                 maxOnline_;
    // 模型工厂
    std::unique_ptr<ModelFactory>                    modelFactory_;
#ifdef ENABLE_TENSORRT
    std::unique_ptr<inference::ConversionManager>    conversionManager_;
#endif
    // 动态批处理（shared_ptr 因为 PredictHandler 需要持有引用）
    std::shared_ptr<RequestBatcher>                  batcher_;
    // 应用配置
    AppConfig                                        config_;
    std::string                                      configPath_;
};
