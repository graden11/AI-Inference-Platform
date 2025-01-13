#pragma once

#include "../Middleware.h"
#include "../../http/HttpRequest.h"
#include "../../http/HttpResponse.h"
#include "CorsConfig.h"

namespace http {
namespace middleware {

class CorsMiddleware : public Middleware {
public:
    explicit CorsMiddleware(const CorsConfig& config = CorsConfig::defaultConfig());
    
    void before(HttpRequest& request) override;
    void after(HttpResponse& response) override;

    std::string join(const std::vector<std::string>& strings, const std::string& delimiter);

private:
    CorsConfig config_;
    //const HttpRequest* request_; // 添加请求引用成员
    
    bool isOriginAllowed(const std::string& origin) const;
    void handlePreflightRequest(const HttpRequest& request, HttpResponse& response);
    void addCorsHeaders(HttpResponse& response, const std::string& origin);
};

} // namespace middleware
} // namespace http