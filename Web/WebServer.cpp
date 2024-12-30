#include "WebServer.h"

WebServer::WebServer(const muduo::net::InetAddress& listenAddr,
                 const std::string& name,
                 muduo::net::TcpServer::Option option)
    : httpServer_(listenAddr, name, option)
{
    httpServer_.setHttpCallback(
        std::bind(&WebServer::onProcess, this, std::placeholders::_1, std::placeholders::_2));
}

void WebServer::setThreadNum(int numThreads)
{
    httpServer_.setThreadNum(numThreads);
}

void WebServer::start()
{
    httpServer_.start();
}

void WebServer::onProcess(const HttpRequest& req, HttpResponse* resp)
{
    std::string reqResource;   
    if (req.method() == HttpRequest::kGet)
    {
        // 判断请求哪个html网页
        if(req.path() == "/login.html")
        {
            reqResource.append("Web/resource/login.html");
        }
        else if (req.path() == "/" || req.path() == "/register.html")
        {
            reqResource.append("Web/resource/register.html");
        }
        else
        {
            reqResource.append("Web/resource");
            reqResource.append(req.path().c_str());
        } 
    }
    else if (req.method() == HttpRequest::kPost)
    {
        if (!req.getBody().empty())
        {
            // 请求体，默认是账号密码, 格式：username=xxx&password=xxx
            std::string username = req.getBody()
            .substr(req.getBody().find("=") + 1, req.getBody().find("&"));
            
            std::string password = req.getBody()
            .substr(req.getBody().find("&") + 1)
            .substr(req.getBody().find("=") + 1); 
            
            // 判断是登录还是注册
            if (req.path() == "/login.cgi")
            {
                // 判断是否存有账号密码
                if(users_.find(username) != users_.end())
                {
                    if (users_[username] == password) // 用户名密码正确
                    {
                        reqResource.append("Web/resource/index.html");
                    }
                    else 
                    {
                        reqResource.append("Web/resource/loginError.html");
                    }
                }
                else
                {
                    reqResource.append("Web/resource/loginError.html");
                }
            }
            else if (req.path() == "/register.cgi")
            {
                // 注册，添加账号密码，判断是否已经存在
                if (users_.find(username) != users_.end())
                {
                    reqResource.append("Web/resource/registerError.html");
                }
                else
                {
                    users_.insert(std::make_pair(username, password));
                    reqResource.append("Web/resource/login.html");
                }
            }
        }
    }
    
    packagingResp(req, resp, reqResource);
}

void WebServer::packagingResp(const HttpRequest& req, HttpResponse* resp,
                               const std::string& reqResource)
{
    FileOperater fileOperater(reqResource);
    if (!fileOperater.isValid()) 
    {
        LOG_WARN << reqResource << " no such file";
        fileOperater.resetDefaultFile();
    }
    
    std::vector<char> buffer(fileOperater.size());
    fileOperater.readFile(buffer); // 读出文件数据
    
    // 封装响应报文
    resp->setVersion(req.getVersion());
    resp->setStatusCode(HttpResponse::k200Ok);
    resp->setStatusMessage("OK");
    resp->setContentLength(fileOperater.size());
    resp->setContentType("text/html");
    resp->setBody(std::string(buffer.data(), buffer.size()));
}