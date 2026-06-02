#include "../include/session/RedisSessionStorage.h"
#include <muduo/base/Logging.h>
#include <hiredis/hiredis.h>

namespace http {
namespace session {

RedisSessionStorage::RedisSessionStorage(const std::string& host, int port)
    : ctx_(nullptr), host_(host), port_(port)
{
    ctx_ = redisConnect(host.c_str(), port);
    if (ctx_ == nullptr || ctx_->err) {
        LOG_ERROR << "Redis connection failed: "
                  << (ctx_ ? ctx_->errstr : "cannot allocate context");
        if (ctx_) redisFree(ctx_);
        ctx_ = nullptr;
    }
}

RedisSessionStorage::~RedisSessionStorage()
{
    if (ctx_) redisFree(ctx_);
}

void RedisSessionStorage::reconnect()
{
    if (ctx_) redisFree(ctx_);
    ctx_ = redisConnect(host_.c_str(), port_);
    if (ctx_ && ctx_->err) {
        LOG_ERROR << "Redis reconnect failed: " << ctx_->errstr;
        redisFree(ctx_);
        ctx_ = nullptr;
    }
}

static std::string jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

static std::string jsonUnescape(const std::string& s) { return s; }

std::string RedisSessionStorage::serialize(const Session& session) const
{
    std::string json = "{\"id\":\"" + jsonEscape(session.getId()) + "\",\"maxAge\":3600,\"data\":{";
    bool first = true;
    for (const auto& [k, v] : session.getData()) {
        if (!first) json += ",";
        first = false;
        json += "\"" + jsonEscape(k) + "\":\"" + jsonEscape(v) + "\"";
    }
    json += "}}";
    return json;
}

std::shared_ptr<Session> RedisSessionStorage::deserialize(const std::string& json) const
{
    // Minimal JSON parser for our simple format: {"id":"...","maxAge":N,"data":{"k":"v",...}}
    auto extractString = [&json](const std::string& key) -> std::string {
        size_t pos = json.find("\"" + key + "\":\"");
        if (pos == std::string::npos) return "";
        pos += key.length() + 4;
        size_t end = json.find("\"", pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    };

    std::string id = extractString("id");
    if (id.empty()) return nullptr;

    auto session = std::make_shared<Session>(id, nullptr, 3600);

    // Parse data sub-object
    size_t dataPos = json.find("\"data\":{");
    if (dataPos == std::string::npos) return session;
    dataPos += 8; // skip "data":{
    size_t dataEnd = json.find("}}", dataPos);
    if (dataEnd == std::string::npos) return session;
    std::string dataStr = json.substr(dataPos, dataEnd - dataPos);

    // Parse "key":"value" pairs
    size_t p = 0;
    while (p < dataStr.size()) {
        if (dataStr[p] != '"') break;
        size_t keyEnd = dataStr.find("\":\"", p + 1);
        if (keyEnd == std::string::npos) break;
        std::string k = dataStr.substr(p + 1, keyEnd - p - 1);
        size_t valStart = keyEnd + 3;
        size_t valEnd = dataStr.find("\"", valStart);
        if (valEnd == std::string::npos) break;
        std::string v = dataStr.substr(valStart, valEnd - valStart);
        session->setValue(k, v);
        p = valEnd + 1;
        if (p < dataStr.size() && dataStr[p] == ',') p++;
    }

    return session;
}

void RedisSessionStorage::save(std::shared_ptr<Session> session)
{
    std::lock_guard<std::mutex> lock(redisMutex_);
    if (!ctx_) reconnect();
    if (!ctx_) return;

    std::string key = "session:" + session->getId();
    std::string val = serialize(*session);

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "SETEX %s %d %b", key.c_str(), 3600, val.data(), val.size()));
    if (reply) freeReplyObject(reply);
}

std::shared_ptr<Session> RedisSessionStorage::load(const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(redisMutex_);
    if (!ctx_) reconnect();
    if (!ctx_) return nullptr;

    std::string key = "session:" + sessionId;
    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "GET %s", key.c_str()));
    if (!reply) return nullptr;

    std::shared_ptr<Session> session;
    if (reply->type == REDIS_REPLY_STRING) {
        std::string json(reply->str, reply->len);
        session = deserialize(json);
    }
    freeReplyObject(reply);
    return session;
}

void RedisSessionStorage::remove(const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(redisMutex_);
    if (!ctx_) reconnect();
    if (!ctx_) return;

    std::string key = "session:" + sessionId;
    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "DEL %s", key.c_str()));
    if (reply) freeReplyObject(reply);
}

} // namespace session
} // namespace http
