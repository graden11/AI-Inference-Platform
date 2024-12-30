#include "../include/handlers/RegisterHandler.h"

void RegisterHandler::handle(const HttpRequest& req, HttpResponse* resp)
{
    // 解析body(json格式)
    json parsed = json::parse(req.getBody());
    std::string username = parsed["username"];
    std::string password = parsed["password"];

    // 判断用户是否已经存在，如果存在则注册失败
    int userId = insertUser(username, password);
    if (userId != -1)
    {
        // 插入成功
        // 封装成功响应
        json successResp;   
        successResp["status"] = "success";
        successResp["message"] = "Register successful";
        successResp["userId"] = userId;
        std::string successBody = successResp.dump(4);

        resp->setStatusLine(req.getVersion(), HttpResponse::k200Ok, "OK");
        resp->setCloseConnection(false);
        resp->setContentType("application/json");
        resp->setContentLength(successBody.size());
        resp->setBody(successBody);
    }
    else
    {
        // 插入失败
        json failureResp;
        failureResp["status"] = "error";
        failureResp["message"] = "username already exists";
        std::string failureBody = failureResp.dump(4);

        resp->setStatusLine(req.getVersion(), HttpResponse::k409Conflict, "Conflict");
        resp->setCloseConnection(false);
        resp->setContentType("application/json");
        resp->setContentLength(failureBody.size());
        resp->setBody(failureBody);
    }
}

int RegisterHandler::insertUser(const std::string& username, const std::string& password)
{
    return server_->insertUser(username, password);
}