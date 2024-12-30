#pragma once
#include "../../../../HttpServer/include/router/RouterHandler.h"
#include "../../../HttpServer/include/utils/MysqlUtil.h"
#include "../GomokuServer.h"
#include "../../../HttpServer/include/utils/JsonUtil.h"


class LoginHandler : public RouterHandler {
public:
    explicit LoginHandler(GomokuServer* server) : server_(server) {}
    
    void handle(const HttpRequest& req, HttpResponse* resp) override;

    void recycleUser(int userId)
    {
        server_->isLogining_.erase(userId);
    }
private:
    int queryUserId(const std::string& username, const std::string& password);

private:
    GomokuServer* server_;
    // MysqlUtil     dbOperator_;
    // userId -> 是否登录
    // std::unordered_map<int, bool> isLogining_; // 但是在这里回收又咋办呢？要不提供回收的接口怎么样
};