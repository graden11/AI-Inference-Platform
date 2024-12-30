#pragma once
#include <string>
#include <memory>
#include "../http/HttpRequest.h"
#include "../http/HttpResponse.h"

class RouterHandler 
{
public:
    virtual ~RouterHandler() = default;
    virtual void handle(const HttpRequest& req, HttpResponse* resp) = 0;
};