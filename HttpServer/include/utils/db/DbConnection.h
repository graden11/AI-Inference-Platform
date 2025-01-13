#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <cppconn/connection.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>
#include <mysql_driver.h>

namespace http 
{
namespace db 
{

class DbConnection 
{
public:
    DbConnection(const std::string& host, 
                const std::string& user,
                const std::string& password,
                const std::string& database);
    ~DbConnection();

    // 禁止拷贝
    DbConnection(const DbConnection&) = delete;
    DbConnection& operator=(const DbConnection&) = delete;

    bool isValid();
    void reconnect();
    void cleanup();

    std::shared_ptr<sql::ResultSet> query(const std::string& sql);
    void execute(const std::string& sql);

    // 新增：使用预处理语句的查询方法
    std::shared_ptr<sql::ResultSet> queryWithParams(
        const std::string& sql,
        const std::vector<std::string>& params = std::vector<std::string>());

    // 新增：执行预处理语句
    void executeWithParams(
        const std::string& sql,
        const std::vector<std::string>& params = std::vector<std::string>());

private:
    // 新增：获取预处理语句
    std::shared_ptr<sql::PreparedStatement> getPreparedStatement(const std::string& sql);
    // 新增：清理过期的预处理语句
    void cleanupStmtCache();

private:
    std::unique_ptr<sql::Connection> conn_;
    std::string host_;
    std::string user_;
    std::string password_;
    std::string database_;
    std::mutex  mutex_;
    std::map<std::string, std::shared_ptr<sql::PreparedStatement>> stmtCache_;
};

} // namespace db
} // namespace http