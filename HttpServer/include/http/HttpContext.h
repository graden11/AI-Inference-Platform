#pragma once

#include <iostream>

#include <muduo/net/TcpServer.h>

#include "HttpRequest.h"

class HttpContext 
{
public:
    enum HttpRequestParseState
    {
        kExpectRequestLine,
        kExpectHeaders,
        kExpectBody,
        kGotAll,
    };
    
    HttpContext()
    : state_(kExpectRequestLine)
    {}

    bool parseRequest(muduo::net::Buffer* buf, muduo::Timestamp receiveTime);
    bool gotAll() const { return state_ == kGotAll; }

    void reset()
    {
        state_ = kExpectRequestLine;
        HttpRequest dummyData;
        request_.swap(dummyData);
    }

    const HttpRequest& request() const
    { return request_;}

    HttpRequest& request()
    { return request_;}

private:
    bool processRequestLine(const char* begin, const char* end);\

private:
    HttpRequestParseState state_;
    HttpRequest           request_;
};
