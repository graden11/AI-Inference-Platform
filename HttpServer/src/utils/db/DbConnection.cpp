#include "../../../include/utils/db/DbConnection.h"
#include "../../../include/utils/db/DbException.h"
#include <muduo/base/Logging.h>

namespace http 
{
namespace db 
{

DbConnection::DbConnection(const std::string& host,
                         const std::string& user,
                         const std::string& password,
                         const std::string& database)
    : host_(host)
    , user_(user)
    , password_(password)
    , database_(database)
{
    try {
        sql::mysql::MySQL_Driver* driver = sql::mysql::get_mysql_driver_instance();
        conn_.reset(driver->connect(host_, user_, password_));
        conn_->setSchema(database_);
        
        // 设置连接属性
        conn_->setClientOption("OPT_RECONNECT", "true");
        conn_->setClientOption("OPT_CONNECT_TIMEOUT", "10");
        conn_->setClientOption("multi_statements", "false");
        
        // 设置字符集
        std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
        stmt->execute("SET NAMES utf8mb4");
        
        LOG_INFO << "Database connection established";
    } catch (const sql::SQLException& e) {
        LOG_ERROR << "Failed to create database connection: " << e.what();
        throw DbException(e.what());
    }
}

std::shared_ptr<sql::PreparedStatement> DbConnection::getPreparedStatement(
    const std::string& sql) 
{
    auto it = stmtCache_.find(sql);
    if (it != stmtCache_.end() && it->second) 
    {
        return it->second;
    }

    std::shared_ptr<sql::PreparedStatement> stmt(conn_->prepareStatement(sql));
    stmtCache_[sql] = stmt;
    return stmt;
}

void DbConnection::cleanupStmtCache() 
{
    stmtCache_.clear();
}

std::shared_ptr<sql::ResultSet> DbConnection::queryWithParams(
    const std::string& sql,
    const std::vector<std::string>& params) 
{
    std::lock_guard<std::mutex> lock(mutex_);
    try 
    {
        if (!isValid()) 
        {
            reconnect();
            cleanupStmtCache();
        }

        auto stmt = getPreparedStatement(sql);
        
        // 设置参数
        for (size_t i = 0; i < params.size(); ++i) 
        {
            stmt->setString(i + 1, params[i]);
        }
        
        // 开始事务
        conn_->setAutoCommit(false);
        
        // 执行查询
        std::shared_ptr<sql::ResultSet> result(stmt->executeQuery());
        
        // 预取所有数据
        std::vector<std::map<std::string, std::string>> rows;
        sql::ResultSetMetaData* meta = result->getMetaData();
        int columns = meta->getColumnCount();
        
        while (result->next()) 
        {
            // 预取数据，确保完全消费结果集
        }
        result->beforeFirst();
        
        // 提交事务
        conn_->commit();
        conn_->setAutoCommit(true);
        
        return result;
    } 
    catch (const sql::SQLException& e) 
    {
        try 
        {
            conn_->rollback();
        } 
        catch (...) 
        {
            // 忽略回滚错误
        }
        conn_->setAutoCommit(true);
        LOG_ERROR << "Query failed: " << e.what() << ", SQL: " << sql;
        throw DbException(e.what());
    }
}

void DbConnection::executeWithParams(
    const std::string& sql,
    const std::vector<std::string>& params) 
{
    std::lock_guard<std::mutex> lock(mutex_);
    try 
    {
        if (!isValid()) {
            reconnect();
            cleanupStmtCache();
        }

        auto stmt = getPreparedStatement(sql);
        
        // 设置参数
        for (size_t i = 0; i < params.size(); ++i) 
        {
            stmt->setString(i + 1, params[i]);
        }
        
        // 开始事务
        conn_->setAutoCommit(false);
        
        // 执行
        stmt->execute();
        
        // 提交事务
        conn_->commit();
        conn_->setAutoCommit(true);
    } 
    catch (const sql::SQLException& e) 
    {
        try 
        {
            conn_->rollback();
        } 
        catch (...) 
        {
            // 忽略回滚错误
        }
        conn_->setAutoCommit(true);
        LOG_ERROR << "Execute failed: " << e.what() << ", SQL: " << sql;
        throw DbException(e.what());
    }
}

DbConnection::~DbConnection() 
{
    cleanup();
    LOG_INFO << "Database connection closed";
}

bool DbConnection::isValid() 
{
    try 
    {
        if (!conn_) return false;
        std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
        stmt->execute("SELECT 1");
        return true;
    } 
    catch (const sql::SQLException&) 
    {
        return false;
    }
}

void DbConnection::reconnect() 
{
    try 
    {
        sql::mysql::MySQL_Driver* driver = sql::mysql::get_mysql_driver_instance();
        conn_.reset(driver->connect(host_, user_, password_));
        conn_->setSchema(database_);
        cleanupStmtCache();
        LOG_INFO << "Database connection reestablished";
    } 
    catch (const sql::SQLException& e) 
    {
        LOG_ERROR << "Failed to reconnect: " << e.what();
        throw DbException(e.what());
    }
}

std::shared_ptr<sql::ResultSet> DbConnection::query(const std::string& sql) 
{
    // 虽然取出连接时已加锁，但为了防止业务层误操作，操作连接时再加锁
    std::lock_guard<std::mutex> lock(mutex_);
    try 
    {
        cleanup();  // 在查询前清理连接状态
        
        if (!isValid()) 
        {
            reconnect();
        }
        // 使用独立的Statement对象
        std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
        std::shared_ptr<sql::ResultSet> result(stmt->executeQuery(sql));

        // 确保获取所有数据
        while (result->next()) 
        {
            // 预取所有数据
        }
        result->beforeFirst(); // 重置结果集指针
        return result;
    } 
    catch (const sql::SQLException& e) 
    {
        LOG_ERROR << "Query failed: " << e.what() << ", SQL: " << sql;
        throw DbException(e.what());
    }
}

void DbConnection::execute(const std::string& sql) 
{
    std::lock_guard<std::mutex> lock(mutex_);
    try 
    {
        if (!isValid()) 
        {
            reconnect();
        }

        // 使用独立的Statement对象
        std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
        stmt->execute(sql);

        // 确保没有未处理的结果集
        while (stmt->getMoreResults())
        {
            // 清空所有结果集
        }
    } 
    catch (const sql::SQLException& e) 
    {
        LOG_ERROR << "Execute failed: " << e.what() << ", SQL: " << sql;
        throw DbException(e.what());
    }
}

void DbConnection::cleanup() 
{
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        cleanupStmtCache();
        if (conn_) 
        {
            // 确保所有事务都已完成
            if (!conn_->getAutoCommit()) 
            {
                conn_->rollback();
                conn_->setAutoCommit(true);
            }
            
            // 清理所有未处理的结果集
            std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
            while (stmt->getMoreResults()) 
            {
                auto result = stmt->getResultSet();
                while (result && result->next()) {
                    // 消费所有结果
                }
            }
        }
    } 
    catch (const std::exception& e) 
    {
        LOG_WARN << "Error cleaning up connection: " << e.what();
        try 
        {
            reconnect();
        } 
        catch (...) 
        {
            // 忽略重连错误
        }
    }
}

} // namespace db
} // namespace http
