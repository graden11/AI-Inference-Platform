#include "../include/handlers/AiGameMoveHandler.h"

void AiGameMoveHandler::handle(const HttpRequest& req, HttpResponse* resp)
{
     try {
        // 解析请求体
        std::cout << "请求体：" << req.getBody() << std::endl; // fixme：这里可能有问题。
        json request = json::parse(req.getBody()); // 就是这里出问题了这个body，它本身就是请求头，它不是真的body
        int userId = std::stoi(request["userId"].get<std::string>());
        int x = request["x"];
        int y = request["y"];

        // 获取或创建游戏实例
        if (server_->aiGames_.find(userId) == server_->aiGames_.end()) {
            server_->aiGames_[userId] = std::make_shared<AiGame>(userId);
        }
        auto& game = server_->aiGames_[userId];

        // 处理人类玩家移动
        if (!game->humanMove(x, y)) 
        {
            json response = {
                {"status", "error"},
                {"message", "Invalid move"}
            };
            std::string responseBody = response.dump();

            resp->setStatusLine(req.getVersion(), HttpResponse::k400BadRequest, "Bad Request");
            resp->setCloseConnection(false);
            resp->setContentType("application/json");
            resp->setContentLength(responseBody.size());
            resp->setBody(responseBody);
            return;
        }

        // 检查人类玩家是否获胜
        if (game->isGameOver()) 
        {
            json response = {
                {"status", "ok"},
                {"board", game->getBoard()},
                {"winner", "human"},
                {"next_turn", "none"}
            };
            std::string responseBody = response.dump();

            resp->setStatusLine(req.getVersion(), HttpResponse::k200Ok, "OK");
            resp->setCloseConnection(false);
            resp->setContentType("application/json");
            resp->setContentLength(responseBody.size());
            resp->setBody(responseBody);

            server_->aiGames_.erase(userId); // 这里删掉以后，每次restart都需要重新创建就行
            return;
        }

        // 检查是否平局（在AI移动之前）
        if (game->isDraw()) 
        {
            json response = {
                {"status", "ok"},
                {"board", game->getBoard()},
                {"winner", "draw"},
                {"next_turn", "none"}
            };
            std::string responseBody = response.dump();

            resp->setStatusLine(req.getVersion(), HttpResponse::k200Ok, "OK");
            resp->setCloseConnection(false);
            resp->setContentType("application/json");
            resp->setContentLength(responseBody.size());
            resp->setBody(responseBody);
                       
            server_->aiGames_.erase(userId);
            return;
        }

        // AI移动
        game->aiMove();

        // 检查AI是否获胜
        if (game->isGameOver()) {
            json response = {
                {"status", "ok"},
                {"board", game->getBoard()},
                {"winner", "ai"},
                {"next_turn", "none"}
            };
            std::string responseBody = response.dump();
            
            resp->setStatusLine(req.getVersion(), HttpResponse::k200Ok, "OK");
            resp->setCloseConnection(false);
            resp->setContentType("application/json");
            resp->setContentLength(responseBody.size());
            resp->setBody(responseBody);

            server_->aiGames_.erase(userId);
            return;
        }

        // 再次检查是否平局（在AI移动之后）
        if (game->isDraw()) {
            json response = {
                {"status", "ok"},
                {"board", game->getBoard()},
                {"winner", "draw"},
                {"next_turn", "none"},
                {"last_move", {{"x", game->getLastMove().first}, {"y", game->getLastMove().second}}}
            };
            std::string responseBody = response.dump();
            
            resp->setStatusLine(req.getVersion(), HttpResponse::k200Ok, "OK");
            resp->setCloseConnection(false);
            resp->setContentType("application/json");
            resp->setContentLength(responseBody.size());
            resp->setBody(responseBody);

            server_->aiGames_.erase(userId);
            return;
        }

        // 游戏继续
        json response = {
            {"status", "ok"},
            {"board", game->getBoard()},
            {"winner", "none"},
            {"next_turn", "human"},
            {"last_move", {{"x", game->getLastMove().first}, {"y", game->getLastMove().second}}}
        };

        std::string responseBody = response.dump();

        resp->setStatusLine(req.getVersion(), HttpResponse::k200Ok, "OK");
            resp->setCloseConnection(false);
            resp->setContentType("application/json");
            resp->setContentLength(responseBody.size());
            resp->setBody(responseBody);
    }
    catch (const std::exception& e) { // 一旦这里出问题，全部乱了套了
        
        json response = {
            {"status", "error"},
            {"message", e.what()}
        };
        std::string responseBody = response.dump();
        server_->packageResp(req.getVersion(), HttpResponse::k500InternalServerError, "Internal Server Error"
                   , false, "application/json", responseBody.size(), responseBody, resp);
    }
}