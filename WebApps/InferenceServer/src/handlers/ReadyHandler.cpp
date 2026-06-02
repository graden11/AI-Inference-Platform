#include "../../include/handlers/ReadyHandler.h"
#include "../../include/ModelFactory.h"

#include <muduo/base/Logging.h>
#include <nlohmann/json.hpp>

#ifdef ENABLE_REDIS
#include <hiredis/hiredis.h>
#endif

void ReadyHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    using json = nlohmann::json;

    bool mysqlOk = false;
    bool redisOk = false;
    bool modelsOk = false;

    // MySQL check
    try
    {
        sql::ResultSet* res = server_->mysqlUtil_.executeQuery("SELECT 1");
        mysqlOk = (res && res->next());
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "Ready check MySQL failed: " << e.what();
    }

    // Redis check: empty host = memory mode = always OK
    if (server_->config_.redis.host.empty())
    {
        redisOk = true;
    }
#ifdef ENABLE_REDIS
    else
    {
        struct timeval timeout = {1, 0};
        redisContext* ctx = redisConnectWithTimeout(
            server_->config_.redis.host.c_str(),
            server_->config_.redis.port, timeout);
        if (ctx && !ctx->err)
        {
            redisOk = true;
            redisFree(ctx);
        }
        else
        {
            if (ctx)
            {
                LOG_ERROR << "Ready check Redis failed: " << ctx->errstr;
                redisFree(ctx);
            }
        }
    }
#else
    else
    {
        redisOk = true; // Redis not compiled in, treat as OK
    }
#endif

    // Models check
    auto* factory = server_->getModelFactory();
    modelsOk = (factory && factory->modelCount() > 0);

    bool allReady = mysqlOk && redisOk && modelsOk;

    json body;
    body["status"] = allReady ? "ready" : "not_ready";
    body["checks"]["mysql"]  = mysqlOk;
    body["checks"]["redis"]  = redisOk;
    body["checks"]["models"] = modelsOk;

    std::string bodyStr = body.dump();
    auto statusCode = allReady ? http::HttpResponse::k200Ok
                               : http::HttpResponse::k503ServiceUnavailable;
    std::string statusMsg = allReady ? "OK" : "Service Unavailable";

    resp->setStatusLine(req.getVersion(), statusCode, statusMsg);
    resp->setContentType("application/json");
    resp->setContentLength(bodyStr.size());
    resp->setBody(bodyStr);
    resp->setCloseConnection(false);
}
