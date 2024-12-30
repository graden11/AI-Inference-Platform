#pragma once

#include <memory>

#include "../HttpServer.h"

#include "../utils/FileOperater.h"

class WebServer
{
public:
    WebServer(const muduo::net::InetAddress& listenAddr,
              const std::string& nameArg,
              muduo::net::TcpServer::Option option = muduo::net::TcpServer::kNoReusePort);

    void setThreadNum(int numThreads);
    void start();

private:
    void onProcess(const HttpRequest& req, HttpResponse* resp);
    void packagingResp(const HttpRequest& req, HttpResponse* resp, const std::string& reqResource);

private:
    HttpServer httpServer_;
    std::unordered_map<std::string, std::string> users_;
};