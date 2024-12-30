#include "../../include/http/HttpServer.h"

#include <any>

// 默认http回应函数
void defaultHttpCallback(const HttpRequest &, HttpResponse *resp)
{
    resp->setStatusCode(HttpResponse::k404NotFound);
    resp->setStatusMessage("Not Found");
    resp->setCloseConnection(true);
}

HttpServer::HttpServer(int port,
                       const std::string &name,
                       muduo::net::TcpServer::Option option)
    : listenAddr_(port), server_(&mainLoop_, listenAddr_, name, option), httpCallback_(std::bind(&HttpServer::onHttpCallback, this, std::placeholders::_1, std::placeholders::_2))
{
    // 设置回调函数
    server_.setConnectionCallback(
        std::bind(&HttpServer::onConnection, this, std::placeholders::_1));
    server_.setMessageCallback(
        std::bind(&HttpServer::onMessage, this,
                  std::placeholders::_1,
                  std::placeholders::_2,
                  std::placeholders::_3));
}

// 服务器运行函数
void HttpServer::start()
{
    LOG_WARN << "HttpServer[" << server_.name() << "] starts listening on" << server_.ipPort();
    server_.start();
    mainLoop_.loop();
}

void HttpServer::onConnection(const muduo::net::TcpConnectionPtr &conn)
{
    // 给连接设置一个HttpContext对象用于解析请求报文，提取封装请求信息
    if (conn->connected())
    {
        conn->setContext(HttpContext());
    }
}

void HttpServer::onMessage(const muduo::net::TcpConnectionPtr &conn,
                           muduo::net::Buffer *buf,
                           muduo::Timestamp receiveTime)
{
    try
    {
        // HttpContext对象用于解析出buf中的请求报文，并把报文的关键信息封装到HttpRequest对象中
        HttpContext *context = boost::any_cast<HttpContext>(conn->getMutableContext());
        if (!context->parseRequest(buf, receiveTime)) // 解析一个http请求
        {
            // 如果解析http报文过程中出错
            conn->send("HTTP/1.1 400 Bad Request\r\n\r\n");
            conn->shutdown();
        }

        // 用户登录成功时绑定tcpConnection和

        // 如果buf缓冲区中解析出一个完整的数据包才封装响应报文
        if (context->gotAll())
        {
            onRequest(conn, context->request());
            context->reset();
        }
    }
    catch (const std::exception &e)
    {
        // 捕获异常，返回错误信息
        LOG_ERROR << "Exception in onMessage: " << e.what();
        conn->send("HTTP/1.1 400 Bad Request\r\n\r\n");
        conn->shutdown();
    }
}

void HttpServer::onRequest(const muduo::net::TcpConnectionPtr &conn, const HttpRequest &req)
{
    const std::string &connection = req.getHeader("Connection");
    bool close = ((connection == "close") ||
                  (req.getVersion() == "HTTP/1.0" && connection != "Keep-Alive"));
    HttpResponse response(close);
    
    // 处理OPTIONS请求
    if (req.method() == HttpRequest::kOptions)
    {
        response.setStatusLine(req.getVersion(), HttpResponse::k200Ok, "OK");
        response.addHeader("Access-Control-Allow-Origin", "*");
        response.addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        response.addHeader("Access-Control-Allow-Headers", "Content-Type");
        response.addHeader("Access-Control-Max-Age", "86400");

        muduo::net::Buffer buf;
        response.appendToBuffer(&buf);
        conn->send(&buf);
        return;
    }


    // 根据请求报文信息来封装响应报文对象
    httpCallback_(req, &response);

    muduo::net::Buffer buf;
    response.appendToBuffer(&buf);

    conn->send(&buf);

    // 如果是短连接的话，返回响应报文后就断开连接
    if (response.closeConnection())
    {
        conn->shutdown();
    }
}

void HttpServer::onHttpCallback(const HttpRequest &req, HttpResponse *resp)
{
    if (!router_.route(req, resp))
    {
        std::cout << "请求的啥，url：" << req.method() << " " << req.path() << std::endl;
        std::cout << "未找到路由，返回404" << std::endl;
        resp->setStatusCode(HttpResponse::k404NotFound);
        resp->setStatusMessage("Not Found");
        resp->setCloseConnection(true);
    }
}