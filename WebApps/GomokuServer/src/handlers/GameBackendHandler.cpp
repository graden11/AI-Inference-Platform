#include "../include/handlers/GameBackendHandler.h"

void GameBackendHandler::handle(const HttpRequest& req, HttpResponse* resp)
{
    // 后台界面
    // 获取当前在线人数、历史最高在线人数、数据库中已注册用户总数
    // int curOnline = server_->getCurOnline();
    // int maxOnline = server_->getMaxOnline();
    // int totalUser = server_->getUserCount();

    // Json::Value respBody;
    // respBody["curOnline"] = curOnline;
    // respBody["maxOnline"] = maxOnline;
    // respBody["totalUser"] = totalUser;
    // std::string respBodyStr = respBody.toStyledString();
    // server_->packageResp(req.getVersion(), HttpResponse::k200Ok, "OK", false, "application/json", respBodyStr.size(), respBodyStr, resp);

    std::string reqFile("../WebApps/GomokuServer/resource/Backend.html");
    FileUtil fileOperater(reqFile);
    if (!fileOperater.isValid())
    {
        LOG_WARN << reqFile << "not exist.";
        fileOperater.resetDefaultFile();
    }

    std::vector<char> buffer(fileOperater.size());
    fileOperater.readFile(buffer); // 读出文件数据
    std::string htmlContent(buffer.data(), buffer.size());

    resp->setStatusLine(req.getVersion(), HttpResponse::k200Ok, "OK");
    resp->setCloseConnection(false);
    resp->setContentType("text/html");
    resp->setContentLength(htmlContent.size());
    resp->setBody(htmlContent);
}
