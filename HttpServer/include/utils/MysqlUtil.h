#pragma once

#include <string>

#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>
#include <mysql_driver.h>
#include <mysql_connection.h>

class MysqlUtil
{
public:
    MysqlUtil()
        : driver_(sql::mysql::get_mysql_driver_instance())
    {   
        // 建立连接
        conn_ = std::unique_ptr<sql::Connection>(driver_->connect("tcp://127.0.0.1:3306", "root", "root"));
        // 选择数据库
        conn_->setSchema("Gomoku");
    }

    std::shared_ptr<sql::ResultSet> query(const std::string& sql)
    {
        // 创建并执行 SQL 查询
        pstmt_.reset(conn_->prepareStatement(sql));
        std::shared_ptr<sql::ResultSet> res(pstmt_->executeQuery());

        return res;
    }

    void operatorDB(const std::string& sql)
    {
        pstmt_.reset(conn_->prepareStatement(sql));
        pstmt_->executeUpdate();
    }

    int insertUser(const std::string& username, const std::string& password)
    {
        std::string sql = "insert into users(username, password) values('" + username + "', '" + password + "')";
        operatorDB(sql);
        std::shared_ptr<sql::ResultSet> res(queryUser(username, password));
        if (res->next())
        {
            return res->getInt("id");
        }
        
        return -1;
    }

    std::shared_ptr<sql::ResultSet> queryUser(const std::string& username, const std::string& password)
    {
        std::string sql = "SELECT * FROM users WHERE username = '" + username + "' AND password = '" + password +"'";
        return query(sql);
    }

    bool isUserExist(const std::string& username)
    {
        std::string sql = "SELECT * FROM users WHERE username = '" + username + "'";
        std::shared_ptr<sql::ResultSet> res(query(sql));

        return res->next();
    }

    // 获取用户总数
    int getUserCount()
    {
        std::string sql = "SELECT COUNT(*) as count FROM users";
        std::shared_ptr<sql::ResultSet> res(query(sql));
        
        if (res->next())
        {
            return res->getInt("count");
        }
        return 0;
    }

    // 再封装一个queryNext和queryAll

private:
    sql::mysql::MySQL_Driver*               driver_;
    std::unique_ptr<sql::Connection>        conn_;
    std::unique_ptr<sql::PreparedStatement> pstmt_;
};


