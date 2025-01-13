 #pragma once
 #include "db/DbConnectionPool.h"
 
#include <string>

// #include <cppconn/prepared_statement.h>
// #include <cppconn/resultset.h>
// #include <mysql_driver.h>
// #include <mysql_connection.h>
// #include <thread>

// // MUDUO日志
// #include <muduo/base/Logging.h>

// class MysqlUtil
// {
// public:
//     MysqlUtil()
//         : driver_(sql::mysql::get_mysql_driver_instance())
//         , stop_(false)
//     {   
//         setupConnection();
//         startHeartbeat();
//         LOG_INFO << "MySQL connection initialized";
//     }

//     ~MysqlUtil() 
//     {
//         stop_ = true;
//         if (heartbeatThread_.joinable()) {
//             heartbeatThread_.join();
//         }
//         LOG_INFO << "MySQL connection closed";
//     }

//     std::shared_ptr<sql::ResultSet> query(const std::string& sql) 
//     {
//         try 
//         {
//             if (!ensureConnected()) 
//             {
//                 throw sql::SQLException("Database connection lost");
//             }
            
//             LOG_DEBUG << "Executing query: " << sql;
//             pstmt_.reset(conn_->prepareStatement(sql));
//             std::shared_ptr<sql::ResultSet> res(pstmt_->executeQuery());
//             return res;
//         } 
//         catch (const sql::SQLException& e) 
//         {
//             LOG_ERROR << "Query failed: " << e.what() << ", SQL: " << sql;
//             throw;
//         }
//     }

//     void operatorDB(const std::string& sql) 
//     {
//         try 
//         {
//             if (!ensureConnected()) 
//             {
//                 throw sql::SQLException("Database connection lost");
//             }
            
//             LOG_DEBUG << "Executing update: " << sql;
//             pstmt_.reset(conn_->prepareStatement(sql));
//             pstmt_->executeUpdate();
//         } 
//         catch (const sql::SQLException& e) 
//         {
//             LOG_ERROR << "Update failed: " << e.what() << ", SQL: " << sql;
//             throw;
//         }
//     }

//     // fixme：这个函数和下面那个函数放在这里不大对，应该是业务代码
//     int insertUser(const std::string& username, const std::string& password)
//     {
//         std::string sql = "insert into users(username, password) values('" + username + "', '" + password + "')";
//         operatorDB(sql);
//         std::shared_ptr<sql::ResultSet> res(queryUser(username, password));
//         if (res->next())
//         {
//             return res->getInt("id");
//         }
//         return -1;
//     }

//     std::shared_ptr<sql::ResultSet> queryUser(const std::string& username, const std::string& password)
//     {
//         // std::string sql = "SELECT * FROM users WHERE username = '" + username + "' AND password = '" + password +"'";
//         // return query(sql);
//         auto result = executeWithRetry([&]() {
//             return query("SELECT * FROM users WHERE username = '" + username + "' AND password = '" + password +"'");
//         });

//         return result;
//     }

//     bool isUserExist(const std::string& username)
//     {
//         // std::string sql = "SELECT * FROM users WHERE username = '" + username + "'";
//         // std::shared_ptr<sql::ResultSet> res(query(sql));
//         auto result = executeWithRetry([&]() {
//             return query("SELECT * FROM users WHERE username = '" + username + "'");
//         });

//         return result->next();
//     }


//     // 再封装一个queryNext和queryAll
// private:
//     void setupConnection()
//     {
//         try
//         {
//             // 建立连接
//             conn_ = std::unique_ptr<sql::Connection>(
//             driver_->connect("tcp://127.0.0.1:3306", "wy", "wy2959002395"));
//             // 选择数据库
//             conn_->setSchema("Gomoku");

//             // 配置连接参数
//             std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
//             stmt->execute("SET SESSION wait_timeout=31536000"); // 1年
//             stmt->execute("SET SESSION interactive_timeout=31536000");
//             stmt->execute("SET NAMES utf8mb4");

//             LOG_INFO << "Database connection established successfully";
//         }
//         catch (const sql::SQLException& e) 
//         {
//             LOG_ERROR << "Failed to setup connection: " << e.what();
//             throw;
//         }
//     }

//     // 心跳检测
//     void startHeartbeat()
//     {
//         heartbeatThread_ = std::thread([this]() {
//             LOG_INFO << "Heartbeat thread started";
//             while (!stop_) 
//             {   // 每30分钟检测一次
//                 std::this_thread::sleep_for(std::chrono::minutes(30));
//                 try 
//                 {   // 如果连接有效，则执行心跳查询（保持连接活跃）
//                     if (conn_ && conn_->isValid()) 
//                     {
//                         std::unique_ptr<sql::Statement> stmt(conn_->createStatement());
//                         stmt->execute("SELECT 1"); // 心跳查询
//                         LOG_DEBUG << "Heartbeat ping successful";
//                     }
//                 } 
//                 catch (const sql::SQLException& e) 
//                 {   // 如果心跳查询失败，则尝试重新连接
//                     LOG_WARN << "Heartbeat failed: " << e.what() << ", attempting reconnection";
//                     try 
//                     {
//                         setupConnection();
//                     } 
//                     catch (const sql::SQLException& reconnectError) 
//                     {
//                         LOG_ERROR << "Heartbeat reconnection failed: " << reconnectError.what();
//                     }
//                 }
//             }
//             LOG_INFO << "Heartbeat thread stopped";
//         });
//     }

//     // 确保连接有效
//     bool ensureConnected() 
//     {
//         try 
//         {
//             if (!conn_ || !conn_->isValid()) 
//             {
//                 LOG_WARN << "Connection lost, attempting to reconnect...";
//                 setupConnection();
//             }
//             return true;
//         } 
//         catch (const sql::SQLException& e) 
//         {
//             LOG_ERROR << "Reconnection failed: " << e.what();
//             return false;
//         }
//     }

//     // 添加错误重试机制的辅助方法
//     template<typename Func>
//     auto executeWithRetry(Func operation, int maxRetries = 3) -> decltype(operation()) 
//     {
//         for (int attempt = 1; attempt <= maxRetries; ++attempt) 
//         {
//             try 
//             {
//                 return operation();
//             } 
//             catch (const sql::SQLException& e) 
//             {
//                 LOG_WARN << "Operation failed (attempt " << attempt << "/" << maxRetries 
//                         << "): " << e.what();
                
//                 if (attempt == maxRetries) 
//                 {
//                     throw;
//                 }
                
//                 std::this_thread::sleep_for(std::chrono::seconds(1));
//                 ensureConnected();
//             }
//         }
//         throw sql::SQLException("Max retries exceeded");
//     }

// private:
//     sql::mysql::MySQL_Driver*               driver_;
//     std::unique_ptr<sql::Connection>        conn_;
//     std::unique_ptr<sql::PreparedStatement> pstmt_;
//     std::thread                             heartbeatThread_;
//     bool                                    stop_;
// };

class MysqlUtil
{
public:
    static void init(const std::string& host, const std::string& user,
                    const std::string& password, const std::string& database,
                    size_t poolSize = 10)
    {
        http::db::DbConnectionPool::getInstance().init(
            host, user, password, database, poolSize);
    }

    std::shared_ptr<sql::ResultSet> query(const std::string& sql)
    {
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        return conn->query(sql);
    }

    std::shared_ptr<sql::ResultSet> queryWithParams(
        const std::string& sql,
        const std::vector<std::string>& params = std::vector<std::string>())
    {
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        return conn->queryWithParams(sql, params);
    }

    void operatorDB(const std::string& sql)
    {
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        conn->execute(sql);
    }

    void executeWithParams(
        const std::string& sql,
        const std::vector<std::string>& params = std::vector<std::string>())
    {
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        conn->executeWithParams(sql, params);
    }
};
