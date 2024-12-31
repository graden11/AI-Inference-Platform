


#include "../include/handlers/EntryHandler.h"
#include "../include/handlers/LoginHandler.h"
#include "../include/handlers/RegisterHandler.h"
#include "../include/handlers/MenuHandler.h"
#include "../include/handlers/AiGameStartHandler.h"
#include "../include/handlers/LogoutHandler.h"
#include "../include/handlers/AiGameMoveHandler.h"
#include "../include/handlers/GameBackendHandler.h"

GomokuServer::GomokuServer(int port,
                 const std::string& name,
                 muduo::net::TcpServer::Option option)
    : httpServer_(port, name, option)
    , maxOnline_(0)
{
    initializeRouter();
}

void GomokuServer::setThreadNum(int numThreads)
{
    httpServer_.setThreadNum(numThreads);
}

void GomokuServer::start()
{
    httpServer_.start();
}

void GomokuServer::initializeRouter()
{
    // 注册url回调处理器
    // 登录注册入口页面
    httpServer_.Get("/", std::make_shared<EntryHandler>(this));
    httpServer_.Get("/entry", std::make_shared<EntryHandler>(this));
    // 登录
    httpServer_.Post("/login", std::make_shared<LoginHandler>(this));
    // 注册
    httpServer_.Post("/register", std::make_shared<RegisterHandler>(this));
    // 登出
    httpServer_.Post("/user/logout", std::make_shared<LogoutHandler>(this));
    // 菜单页面
    httpServer_.Post("/menu", std::make_shared<MenuHandler>(this));
    // 开始对战ai
    httpServer_.Post("/aiBot/start", std::make_shared<AiGameStartHandler>(this));
    // 下棋
    httpServer_.Post("/aiBot/move", std::make_shared<AiGameMoveHandler>(this));
    // 重新开始对战ai
    httpServer_.Post("/aiBot/restart", std::bind(&GomokuServer::restartChessGameVsAi, this, std::placeholders::_1, std::placeholders::_2));
    // 后台界面
    httpServer_.Get("/backend", std::make_shared<GameBackendHandler>(this));
    // 后台数据获取
    httpServer_.Get("/backend_data", std::bind(&GomokuServer::getBackendData, this, std::placeholders::_1, std::placeholders::_2));
    
    // 注册回调函数
    // router_.registerCallback("/logout", 
    //     [this](const HttpRequest& req, HttpResponse& resp) {
    //         // 处理登出逻辑
    //     });
}

void GomokuServer::restartChessGameVsAi(const HttpRequest& req, HttpResponse* resp)
{
    // 解析请求体
    json request = json::parse(req.getBody());
    int userId = std::stoi(request["userId"].get<std::string>());

    if (aiGames_.find(userId) != aiGames_.end())
        aiGames_.erase(userId);
    aiGames_[userId] = std::make_shared<AiGame>(userId);

    json successResp;
    successResp["status"] = "ok";
    successResp["message"] = "restart successful";
    successResp["userId"] = userId;
    std::string successBody = successResp.dump(4);
    packageResp(req.getVersion(), HttpResponse::k200Ok, "OK", false
               , "application/json", successBody.size(), successBody, resp);
}

// 获取后台数据
void GomokuServer::getBackendData(const HttpRequest& req, HttpResponse* resp)
{
    // 后台界面
    // 获取当前在线人数、历史最高在线人数、数据库中已注册用户总数
    int curOnline = getCurOnline();
    int maxOnline = getMaxOnline();
    int totalUser = getUserCount();

    json respBody;
    respBody["curOnline"] = curOnline;
    respBody["maxOnline"] = maxOnline;
    respBody["totalUser"] = totalUser;
    std::string successBody = respBody.dump(4);
    packageResp(req.getVersion(), HttpResponse::k200Ok, "OK", false,
              "application/json", successBody.size(), successBody, resp);
}

/**/
void GomokuServer::packageResp(const std::string& version, HttpResponse::HttpStatusCode statusCode,
                     const std::string& statusMsg, bool close, const std::string& contentType,
                     int contentLen, const std::string& body, HttpResponse* resp)
{
    resp->setVersion(version);
    resp->setStatusCode(statusCode);    
    resp->setStatusMessage(statusMsg);
    resp->setCloseConnection(close);
    resp->setContentType(contentType);
    resp->setContentLength(contentLen);
    resp->setBody(body);
}


int GomokuServer::queryUserId(const std::string& username, const std::string& password)
{
    // 账号密码都拿到了，查找数据库是否有该账号密码
    std::shared_ptr<sql::ResultSet> res = dbOperator_.queryUser(username, password);
    if (res->next())
    {
        return res->getInt("id");
    }
    
    return -1;
}

int GomokuServer::insertUser(const std::string& username, const std::string& password)
{
    // 判断用户是否存在，如果存在则返回-1，否则返回用户id
    if (!dbOperator_.isUserExist(username)) 
    {
        std::cout << "用户不存在, 执行插入操作" << std::endl;
        // 用户不存在，插入用户
        return dbOperator_.insertUser(username, password);
    }
    
    return -1;
}

