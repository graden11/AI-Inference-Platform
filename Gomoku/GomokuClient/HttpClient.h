#pragma once

#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/StreamCopier.h>

enum class HttpMethod
{
    kGet,
    kPost,
    kInvalid
};

// 这里其实可以把Request和Response分开单独封装成一个类
// 但是为了方便，就直接合在一起了
class HttpClient
{
public:
    HttpClient(const std::string& ip, int port)
        : session_(ip, port)
    {
    }

    template<typename T>
    void addJsonData(const std::string& key, T value)
    {
        jsonObj_.set(key, value);
    }

    std::string jsonDataToString()
    {
        std::ostringstream jsonStream;
        jsonObj_.stringify(jsonStream);
        jsonObj_.clear();
        return jsonStream.str();
    }

    void packageRequestLine(HttpMethod method, const std::string& path)
    {
        // HTTP requestLine
        if (method == HttpMethod::kGet)
        {
            request_ = std::make_unique<Poco::Net::HTTPRequest>(Poco::Net::HTTPRequest::HTTP_GET, path);
        }
        else if (method == HttpMethod::kPost)
        {
            request_ = std::make_unique<Poco::Net::HTTPRequest>(Poco::Net::HTTPRequest::HTTP_POST, path);
        }
    }

    void setContentType(const std::string& contentType)
    {
        request_->setContentType(contentType);
    }

    void setContentLength(uint64_t length)
    {
        request_->setContentLength(length);
    }

    void setCloseConnection(bool on)
    {
        request_->setKeepAlive(!on);
    }

    // get请求调用接口
    void sendRequest()
    {
        // Send request
        std::ostream &os = session_.sendRequest(*request_);
        
        request_.reset(); // 将请求发送出去后，清除当前请求头部的数据
    }

    // post请求调用接口
    void sendRequest(const std::string& requestBody)
    {
        // Send request
        std::ostream &os = session_.sendRequest(*request_);
        os << requestBody;
        
        request_.reset(); // 将请求发送出去后，清除当前请求头部的数据
    }

    // 返回响应体以字符串形式
    void receiveResponse()
    {
        // Receive response
        response_ = std::make_unique<Poco::Net::HTTPResponse>();
        std::istream &is = session_.receiveResponse(*response_);
        std::ostringstream responseStream;
        Poco::StreamCopier::copyStream(is, responseStream);

        responseBodyStr_.clear();
        // 从ostringstream获取字符串
        responseBodyStr_ = responseStream.str(); // 以字符串形式

        // 解析JSON字符串
        Poco::JSON::Parser parser;
        Poco::Dynamic::Var result = parser.parse(responseBodyStr_);
        // 将解析结果转换为Poco::JSON::Object
        pRespBodyJson_ = result.extract<Poco::JSON::Object::Ptr>();
    }

    int getStatusCode()
    {
        // 获取状态码
        return response_->getStatus();
    }

    std::string getJsonBody(const std::string& key)
    {
        // 获取JSON响应体
        return pRespBodyJson_->get(key).toString();
    }

private:
    Poco::Net::HTTPClientSession             session_;
    Poco::JSON::Object                       jsonObj_; // 因为是单线程，所以才能肆无忌惮的放这里
    std::unique_ptr<Poco::Net::HTTPRequest>  request_;
    std::unique_ptr<Poco::Net::HTTPResponse> response_;
    std::string                              responseBodyStr_;
    Poco::JSON::Object::Ptr                  pRespBodyJson_;
};