#pragma once

#include <map>
#include <string>
#include <unordered_map>

#include <muduo/base/Timestamp.h>

class HttpRequest
{
public:
    enum Method
    {
        kInvalid, kGet, kPost, kHead, kPut, kDelete, kOptions
    };
    
    HttpRequest()
        : method_(kInvalid)
        , version_("Unknown")
    {
    }
    
    void setReceiveTime(muduo::Timestamp t);
    muduo::Timestamp receiveTime() const { return receiveTime_; }
    
    bool setMethod(const char* start, const char* end);
    Method method() const { return method_; }

    void setPath(const char* start, const char* end);
    std::string path() const { return path_; }
    
    void setArgument(const char* start, const char* end);
    std::string getArgument(const std::string& key) const { return arguments_.at(key);  }
    
    void setVersion(std::string v)
    {
        version_ = v;
    }

    std::string getVersion() const
    {
        return version_;
    }
    
    void addHeader(const char* start, const char* colon, const char* end);
    std::string getHeader(const std::string& field) const;

    const std::map<std::string, std::string>& headers() const
    { return headers_; }

    void setBody(const std::string& body) { content_ = body; }
    void setBody(const char* start, const char* end) 
    { 
        if (end >= start) {
            content_.assign(start, end - start); 
        }
    }
    
    std::string getBody() const
    { return content_; }

    void setContentLength(uint64_t length)
    { contentLength_ = length; }
    uint64_t contentLength() const
    { return contentLength_; }

    void swap(HttpRequest& that);

private:
    Method                                       method_;
    std::string                                  version_;
    std::string                                  path_;
    std::unordered_map<std::string, std::string> arguments_;
    muduo::Timestamp                             receiveTime_;
    std::map<std::string, std::string>           headers_;
    std::string                                  content_;
    uint64_t                                     contentLength_ { 0 };
};  


