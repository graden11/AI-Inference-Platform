#include "../include/handlers/LoginHandler.h"
#include "../../../../third_party/bcrypt.h"

void LoginHandler::handle(const http::HttpRequest &req, http::HttpResponse *resp)
{
    // 处理登录逻辑
    // 验证 contentType
    auto contentType = req.getHeader("Content-Type");
    if (contentType.empty() || contentType != "application/json" || req.getBody().empty())
    {
        LOG_INFO << "content" << req.getBody();
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
        resp->setCloseConnection(true);
        resp->setContentType("application/json");
        resp->setContentLength(0);
        resp->setBody("");
        return;
    }

    // JSON 解析使用 try catch 捕获异常
    try
    {
        json parsed = json::parse(req.getBody());
        std::string username = parsed["username"];
        std::string password = parsed["password"];
        // 验证用户是否存在
        int userId = queryUserId(username, password);
        if (userId != -1)
        {
            // 获取会话
            auto session = server_->getSessionManager()->getSession(req, resp);
            // 会话都不是同一个会话，因为会话判断是不是同一个会话是通过请求报文中的cookie来判断的
            // 所以不同页面的访问是不可能是相同的会话的，只有该页面前面访问过服务端，才会有会话记录
            // 那么判断用户是否在其他地方登录中不能通过会话来判断
            
            // 在会话中存储用户信息
            session->setValue("userId", std::to_string(userId));
            session->setValue("username", username);
            session->setValue("isLoggedIn", "true");
            int onlineCount = 0;
            bool alreadyOnline = false;
            {
                std::lock_guard<std::mutex> lock(server_->mutexForOnlineUsers_);
                alreadyOnline = server_->onlineUsers_.find(userId) != server_->onlineUsers_.end()
                                && server_->onlineUsers_[userId] == true;
                server_->onlineUsers_[userId] = true;
                onlineCount = static_cast<int>(server_->onlineUsers_.size());
            }

            if (alreadyOnline)
            {
                std::lock_guard<std::mutex> lock(server_->mutexForLoginSessions_);
                auto it = server_->loginSessions_.find(userId);
                if (it != server_->loginSessions_.end())
                    server_->getSessionManager()->destroySession(it->second);
            }

            {
                std::lock_guard<std::mutex> lock(server_->mutexForLoginSessions_);
                server_->loginSessions_[userId] = session->getId();
            }
            server_->updateMaxOnline(onlineCount);

            json successResp;
            successResp["success"] = true;
            successResp["userId"] = userId;
            std::string successBody = successResp.dump(4);

            resp->setStatusLine(req.getVersion(), http::HttpResponse::k200Ok, "OK");
            resp->setCloseConnection(false);
            resp->setContentType("application/json");
            resp->setContentLength(successBody.size());
            resp->setBody(successBody);
            return;
        }
        else // 账号密码错误，请重新登录
        {
            // 封装json数据
            json failureResp;
            failureResp["status"] = "error";
            failureResp["message"] = "Invalid username or password";
            std::string failureBody = failureResp.dump(4);

            resp->setStatusLine(req.getVersion(), http::HttpResponse::k401Unauthorized, "Unauthorized");
            resp->setCloseConnection(false);
            resp->setContentType("application/json");
            resp->setContentLength(failureBody.size());
            resp->setBody(failureBody);
            return;
        }
    }
    catch (const std::exception &e)
    {
        // 捕获异常，返回错误信息
        json failureResp;
        failureResp["status"] = "error";
        failureResp["message"] = e.what();
        std::string failureBody = failureResp.dump(4);

        resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
        resp->setCloseConnection(true);
        resp->setContentType("application/json");
        resp->setContentLength(failureBody.size());
        resp->setBody(failureBody);
        return;
    }
}

int LoginHandler::queryUserId(const std::string &username, const std::string &password)
{
    sql::ResultSet* res = mysqlUtil_.executeQuery(
        "SELECT id, password FROM users WHERE username = ?", username);
    if (res->next())
    {
        int id = res->getInt("id");
        std::string hash = res->getString("password");
        if (bcrypt::validatePassword(password, hash))
        {
            return id;
        }
    }
    return -1;
}

