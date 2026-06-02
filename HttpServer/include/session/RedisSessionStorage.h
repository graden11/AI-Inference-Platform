#pragma once

#include "SessionStorage.h"
#include <mutex>
#include <string>
#include <hiredis/hiredis.h>

namespace http {
namespace session {

// Redis-backed session storage. Sessions are serialized as JSON and stored
// with TTL matching the session maxAge, so expired sessions are auto-purged.
class RedisSessionStorage : public SessionStorage {
public:
    RedisSessionStorage(const std::string& host, int port);
    ~RedisSessionStorage() override;

    void save(std::shared_ptr<Session> session) override;
    std::shared_ptr<Session> load(const std::string& sessionId) override;
    void remove(const std::string& sessionId) override;

private:
    redisContext* ctx_;
    std::string host_;
    int port_;
    std::mutex redisMutex_;

    void reconnect();
    std::string serialize(const Session& session) const;
    std::shared_ptr<Session> deserialize(const std::string& json) const;
};

} // namespace session
} // namespace http
