#include "../../include/router/Router.h"
#include <muduo/base/Logging.h>

void Router::registerHandler(HttpRequest::Method method, const std::string& path, HandlerPtr handler) 
{
    RouteKey key{method, path};
    handlers_[key] = std::move(handler);
}

void Router::registerCallback(HttpRequest::Method method, const std::string& path, const HandlerCallback& callback) 
{
    RouteKey key{method, path};
    callbacks_[key] = std::move(callback);
}

bool Router::route(const HttpRequest& req, HttpResponse* resp) 
{
    RouteKey key{req.method(), req.path()};

    // 先查找处理器
    auto handlerIt = handlers_.find(key);
    if (handlerIt != handlers_.end()) 
    {
        handlerIt->second->handle(req, resp);
        return true;
    }
    
    // 再查找回调函数
    auto callbackIt = callbacks_.find(key);
    if (callbackIt != callbacks_.end()) 
    {
        callbackIt->second(req, resp);
        return true;
    }
    
    return false;
}