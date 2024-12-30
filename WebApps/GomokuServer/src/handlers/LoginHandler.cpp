#include "../include/handlers/LoginHandler.h"

void LoginHandler::handle(const HttpRequest &req, HttpResponse *resp)
{
    // 处理登录逻辑
    // 解析body(json格式)
    // 验证 contentType
    auto contentType = req.getHeader("Content-Type");
    if (contentType.empty() || contentType != "application/json" || req.getBody().empty())
    {
        std::cout << "content" << req.getBody() << req.getBody();
        resp->setStatusLine(req.getVersion(), HttpResponse::k400BadRequest, "Bad Request");
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

        std::cout << "解析成功打印一下：username: " << username << " password: " << password << std::endl;
        int userId = queryUserId(username, password);
        if (userId != -1)
        {
            // 判断当前用户是否在登录中
            if (server_->isLogining_.find(userId) == server_->isLogining_.end() || server_->isLogining_[userId] == false)
            {
                server_->isLogining_[userId] = true;
                // 更新历史最高在线人数
                server_->updateMaxOnline(server_->isLogining_.size());
                // 用户存在登录成功
                // 封装json 数据。
                json successResp;
                successResp["success"] = true;
                successResp["userId"] = userId;
                std::string successBody = successResp.dump(4);

                resp->setStatusLine(req.getVersion(), HttpResponse::k200Ok, "OK");
                resp->setCloseConnection(false);
                resp->setContentType("application/json");
                resp->setContentLength(successBody.size());
                resp->setBody(successBody);
                return;
            }
            else
            {
                // FIXME: 当前该用户正在其他地方登录中，将原有登录用户强制下线更好
                // 不允许重复登录，
                json failureResp;
                failureResp["success"] = false;
                failureResp["error"] = "账号已在其他地方登录";
                std::string failureBody = failureResp.dump(4);

                resp->setStatusLine(req.getVersion(), HttpResponse::k403Forbidden, "Forbidden");
                resp->setCloseConnection(true);
                resp->setContentType("application/json");
                resp->setContentLength(failureBody.size());
                resp->setBody(failureBody);
                return;
            }
        }
        else // 账号密码错误，请重新登录
        {
            // 封装json数据
            json failureResp;
            failureResp["status"] = "error";
            failureResp["message"] = "Invalid username or password";
            std::string failureBody = failureResp.dump(4);

            resp->setStatusLine(req.getVersion(), HttpResponse::k401Unauthorized, "Unauthorized");
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

        resp->setStatusLine(req.getVersion(), HttpResponse::k400BadRequest, "Bad Request");
        resp->setCloseConnection(true);
        resp->setContentType("application/json");
        resp->setContentLength(failureBody.size());
        resp->setBody(failureBody);
        return;
    }
}

int LoginHandler::queryUserId(const std::string &username, const std::string &password)
{
    // 账号密码都拿到了，查找数据库是否有该账号密码
    return server_->queryUserId(username, password);
}