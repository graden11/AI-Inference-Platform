#pragma once

#include <iostream>
#include <memory>

#include <muduo/net/TcpServer.h>

#include "HttpRequest.h"

namespace http
{

struct PerfTrace;  // fwd decl — full definition in HttpResponse.h

class HttpContext
{
public:
    enum HttpRequestParseState
    {
        kExpectRequestLine, // 解析请求行
        kExpectHeaders, // 解析请求头
        kExpectBody, // 解析请求体
        kGotAll, // 解析完成
    };

    HttpContext()
    : state_(kExpectRequestLine), maxBodySize_(10 * 1024 * 1024)  // 10 MB default
    {}

    void setMaxBodySize(uint64_t bytes) { maxBodySize_ = bytes; }

    bool parseRequest(muduo::net::Buffer* buf, muduo::Timestamp receiveTime);
    bool gotAll() const
    { return state_ == kGotAll;  }

    void reset()
    {
        state_ = kExpectRequestLine;
        HttpRequest dummyData;
        request_.swap(dummyData);
        perfTrace_.reset();
    }

    const HttpRequest& request() const
    { return request_;}

    HttpRequest& request()
    { return request_;}

    void setPerfTrace(std::shared_ptr<PerfTrace> pt) { perfTrace_ = std::move(pt); }
    std::shared_ptr<PerfTrace> getPerfTrace() const { return perfTrace_; }

private:
    bool processRequestLine(const char* begin, const char* end);
private:
    HttpRequestParseState state_;
    HttpRequest           request_;
    uint64_t              maxBodySize_ = 10 * 1024 * 1024;
    std::shared_ptr<PerfTrace> perfTrace_;
};

} // namespace http