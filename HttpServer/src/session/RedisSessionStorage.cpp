#include "../include/session/RedisSessionStorage.h"
#include <muduo/base/Logging.h>
#include <nlohmann/json.hpp>
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
    // Exponential backoff: 1s, 2s, 4s, 8s, 16s, max 30s between attempts
    static constexpr auto kMinBackoff = std::chrono::seconds(1);
    static constexpr auto kMaxBackoff = std::chrono::seconds(30);

    auto now = std::chrono::steady_clock::now();
    if (reconnectFailures_ > 0) {
        auto delay = std::min(kMinBackoff * (1 << (reconnectFailures_ - 1)), kMaxBackoff);
        if (now - lastReconnectAttempt_ < delay)
            return; // Not yet due for a retry
    }

    lastReconnectAttempt_ = now;
    if (ctx_) redisFree(ctx_);
    ctx_ = redisConnect(host_.c_str(), port_);
    if (ctx_ && ctx_->err) {
        LOG_ERROR << "Redis reconnect failed: " << ctx_->errstr;
        redisFree(ctx_);
        ctx_ = nullptr;
        reconnectFailures_++;
    } else if (ctx_) {
        if (reconnectFailures_ > 0)
            LOG_INFO << "Redis reconnected after " << reconnectFailures_ << " failures";
        reconnectFailures_ = 0;
    } else {
        reconnectFailures_++;
    }
}

std::string RedisSessionStorage::serialize(const Session& session) const
{
    nlohmann::json j;
    j["id"] = session.getId();
    j["maxAge"] = 3600;
    nlohmann::json data = nlohmann::json::object();
    for (const auto& [k, v] : session.getData())
        data[k] = v;
    j["data"] = data;
    return j.dump();
}

std::shared_ptr<Session> RedisSessionStorage::deserialize(const std::string& jsonStr) const
{
    try {
        nlohmann::json j = nlohmann::json::parse(jsonStr);
        std::string id = j.value("id", "");
        if (id.empty()) return nullptr;
        auto session = std::make_shared<Session>(id, nullptr, j.value("maxAge", 3600));
        if (j.contains("data") && j["data"].is_object()) {
            for (const auto& [k, v] : j["data"].items()) {
                if (v.is_string())
                    session->setValue(k, v.get<std::string>());
            }
        }
        return session;
    } catch (const nlohmann::json::exception& e) {
        LOG_ERROR << "Redis session deserialize failed: " << e.what();
        return nullptr;
    }
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

std::vector<std::string> RedisSessionStorage::getActiveIds()
{
    std::lock_guard<std::mutex> lock(redisMutex_);
    if (!ctx_) reconnect();
    if (!ctx_) return {};

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "KEYS session:*"));
    std::vector<std::string> ids;
    if (reply && reply->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i < reply->elements; ++i) {
            std::string key(reply->element[i]->str, reply->element[i]->len);
            if (key.size() > 8)  // strip "session:" prefix
                ids.push_back(key.substr(8));
        }
    }
    if (reply) freeReplyObject(reply);
    return ids;
}

} // namespace session
} // namespace http
