#pragma once 

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <functional>
#include <iostream>
#include <memory>
#include <unordered_map>

#include <muduo/net/TcpServer.h>
#include <muduo/net/EventLoop.h>
#include <muduo/base/Logging.h>

#include "HttpContext.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "router/Router.h"

class HttpRequest;
class HttpResponse;

class HttpServer : muduo::noncopyable
{
public:
    using HttpCallback = std::function<void (const HttpRequest&, HttpResponse*)>;
    

    HttpServer(int port,
               const std::string& name,
               muduo::net::TcpServer::Option option = muduo::net::TcpServer::kNoReusePort);
    
    void setThreadNum(int numThreads)
    {
        server_.setThreadNum(numThreads);
    }

    void start();

    muduo::net::EventLoop* getLoop() const 
    { 
        return server_.getLoop(); 
    }

    void setHttpCallback(const HttpCallback& cb)
    {
        httpCallback_ = cb;
    }

    // 我感觉不要重写onProcess，直接在HttpServer中注册路由处理器
    void Get(const std::string& path, const HttpCallback& cb)
    {
        router_.registerCallback(HttpRequest::kGet, path, cb);
    }
    
    // 注册路由处理器(ps: 这里是一个多态的使用)
    void Get(const std::string& path, Router::HandlerPtr handler)
    {
        router_.registerHandler(HttpRequest::kGet, path, handler);
    }

    void Post(const std::string& path, const HttpCallback& cb)
    {
        router_.registerCallback(HttpRequest::kPost, path, cb);
    }

    void Post(const std::string& path, Router::HandlerPtr handler)
    {
        router_.registerHandler(HttpRequest::kPost, path, handler);
    }

private:
    void onConnection(const muduo::net::TcpConnectionPtr& conn);
    void onMessage(const muduo::net::TcpConnectionPtr& conn,
                   muduo::net::Buffer* buf,
                   muduo::Timestamp receiveTime);
    void onRequest(const muduo::net::TcpConnectionPtr&, const HttpRequest&);

    void onHttpCallback(const HttpRequest& req, HttpResponse* resp);
    
private:
    muduo::net::InetAddress                      listenAddr_;
    muduo::net::TcpServer                        server_;
    muduo::net::EventLoop                        mainLoop_;
    HttpCallback                                 httpCallback_;  
    Router                                       router_;
}; 


