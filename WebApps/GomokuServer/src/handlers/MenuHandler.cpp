#include "../include/handlers/MenuHandler.h"

void MenuHandler::handle(const HttpRequest &req, HttpResponse *resp)
{
    auto contentType = req.getHeader("Content-Type");
    if (contentType.empty() || contentType != "application/json" || req.getBody().empty())
    {
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
        json requestBody = json::parse(req.getBody());
        int userId = std::stoi(requestBody["userId"].get<std::string>());

        std::string reqFile("../WebApps/GomokuServer/resource/menu.html");
        FileUtil fileOperater(reqFile);
        if (!fileOperater.isValid())
        {
            LOG_WARN << reqFile << "not exist.";
            fileOperater.resetDefaultFile();
        }

        std::vector<char> buffer(fileOperater.size());
        fileOperater.readFile(buffer); // 读出文件数据
        std::string htmlContent(buffer.data(), buffer.size());

        // 在HTML内容中插入userId
        size_t headEnd = htmlContent.find("</head>");
        if (headEnd != std::string::npos)
        {
            std::string script = "<script>const userId = '" + std::to_string(userId) + "';</script>";
            htmlContent.insert(headEnd, script);
        }

        // server_->packageResp(req.getVersion(), HttpResponse::k200Ok, "OK"
        //             , false, "text/html", htmlContent.size(), htmlContent, resp);
        resp->setStatusLine(req.getVersion(), HttpResponse::k200Ok, "OK");
        resp->setCloseConnection(false);
        resp->setContentType("text/html");
        resp->setContentLength(htmlContent.size());
        resp->setBody(htmlContent);
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
    }
}
