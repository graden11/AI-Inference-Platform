#include "../../../include/utils/db/DbConnectionPool.h"
#include "../../../include/utils/db/DbException.h"
#include <muduo/base/Logging.h>

namespace http {
namespace db {

void DbConnectionPool::init(const std::string& host,
                          const std::string& user,
                          const std::string& password,
                          const std::string& database,
                          size_t poolSize) 
{
    // 连接池会被多个线程访问，所以操作其成员变量时需要加锁
    std::lock_guard<std::mutex> lock(mutex_);
    // 确保只初始化一次
    if (initialized_) 
    {
        return;
    }

    host_ = host;
    user_ = user;
    password_ = password;
    database_ = database;

    // 创建连接
    for (size_t i = 0; i < poolSize; ++i) 
    {
        connections_.push(createConnection());
    }

    initialized_ = true;
    LOG_INFO << "Database connection pool initialized with " << poolSize << " connections";
}

DbConnectionPool::~DbConnectionPool() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!connections_.empty()) {
        connections_.pop();
    }
    LOG_INFO << "Database connection pool destroyed";
}

std::shared_ptr<DbConnection> DbConnectionPool::getConnection() 
{
    std::unique_lock<std::mutex> lock(mutex_);
    while (connections_.empty()) 
    {   
        // 如果连接池为空，等待5秒，如果5秒后连接池仍然为空，则抛出异常
        if (cv_.wait_for(lock, std::chrono::seconds(5)) == std::cv_status::timeout) {
            LOG_WARN << "Timeout waiting for database connection";
            throw DbException("Timeout waiting for database connection");
        }
    }

    auto conn = connections_.front();
    connections_.pop();

    // 检查连接是否有效，如果无效则重新创建
    if (!conn->isValid()) 
    {
        try 
        {
            conn = createConnection();
        } 
        catch (const DbException& e) 
        {
            connections_.push(conn);  // 放回无效连接
            throw;
        }
    }

    conn->cleanup();

    // 使用自定义删除器，确保连接返回连接池
    // 这里返回的是新创建的指向DbConnection的智能指针，而不是conn，但是操作的连接还是一样的
    return std::shared_ptr<DbConnection>(conn.get(), 
        [this, conn](DbConnection*) {
            try {
                // 清理连接状态
                conn->cleanup();  // 需要在 DbConnection 中添加此方法
                
                std::lock_guard<std::mutex> lock(mutex_);
                connections_.push(conn);
                cv_.notify_one();
            } catch (const std::exception& e) {
                LOG_ERROR << "Error returning connection to pool: " << e.what();
            }
        });
}

std::shared_ptr<DbConnection> DbConnectionPool::createConnection() {
    return std::make_shared<DbConnection>(host_, user_, password_, database_);
}

} // namespace db
} // namespace http