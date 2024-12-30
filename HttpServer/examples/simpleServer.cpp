#include <iostream>
#include <memory>
#include "../include/http/HttpServer.h"
#include "../include/http/HttpRequest.h"
#include "../include/http/HttpResponse.h"
#include "../include/router/Router.h"


// 示例处理器类
class HelloWorldHandler : public RouterHandler {
public:
    void handle(const HttpRequest& req, HttpResponse* resp) override {
        // 处理 GET 请求
        if (req.method() == HttpRequest::kGet) 
        {
            resp->setStatusCode(HttpResponse::k200Ok);
            resp->setStatusMessage("OK");
            resp->setCloseConnection(true);
            resp->setBody("Hello, World!");
        } else 
        {
            resp->setStatusCode(HttpResponse::k404NotFound);
            resp->setStatusMessage("Not Found");
            resp->setCloseConnection(true);
            resp->setBody("Method Not Allowed");
        }
    }
};

class EchoHandler : public RouterHandler {
public:
    void handle(const HttpRequest& req, HttpResponse* resp) override {
        // 处理 POST 请求
        if (req.method() == HttpRequest::kPost) 
        {
            resp->setStatusCode(HttpResponse::k200Ok);
            resp->setBody("Echo: " + req.getBody());
        } 
        else 
        {
            resp->setStatusCode(HttpResponse::k404NotFound);
            resp->setStatusMessage("Not Found");
            resp->setCloseConnection(true);
            resp->setBody("Method Not Allowed");
        }
    }
};

int main() {

    // 创建 HTTP 服务器
    HttpServer server(8888, "HttpServer"); // 监听8080端口

    server.Get("/hello", std::make_shared<HelloWorldHandler>());
    server.Post("/echo", std::make_shared<EchoHandler>());
    
    server.setThreadNum(4);
    server.start();

    return 0;
}