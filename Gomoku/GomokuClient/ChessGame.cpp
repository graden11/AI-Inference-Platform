#include "ChessGame.h"


void ChessGame::init(int uesrId, std::shared_ptr<HttpClient> spHttpClient)
{
    userId_ = uesrId;
    curGameType_ = NO_GAME;
    spHttpClient_ = spHttpClient;
    
    // 注册信号处理函数，捕获 SIGINT(Ctrl+C)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signalHandler;
    sigaction(SIGINT, &sa, nullptr);
}

void ChessGame::signalHandler(int signum)
{
    // 执行清理逻辑，向服务端发送http请求
    sendReleaseRequest();

    // 退出程序
    std::cout << "\nExiting the Gomoku Game. Goodbye!\n";
    exit(signum);
}

void ChessGame::sendReleaseRequest()
{
    // 客户端主动断开连接服务端一定要知道，因为不断是单机还是多人都得
    // 服务端一定在知道某个客户端断开连接后，都得进行相应的处理，不然双方联机更加麻烦
    // 发送让服务端释放相应资源的请求
    getInstance().spHttpClient_->addJsonData("userId", getInstance().getUserId());
    getInstance().spHttpClient_->addJsonData("gameType", getInstance().getCurGameType()); // 根据不同游戏类型，回收不同资源
    std::string requestBody = getInstance().spHttpClient_->jsonDataToString();
    
    getInstance().spHttpClient_->packageRequestLine(HttpMethod::kPost, "/release");
    getInstance().spHttpClient_->setContentType("application/json");
    getInstance().spHttpClient_->setCloseConnection(true);
    getInstance().spHttpClient_->setContentLength(requestBody.size());
    // send request
    getInstance().spHttpClient_->sendRequest(requestBody);

    // receive response
    getInstance().spHttpClient_->receiveResponse();

    // 获取响应状态码
    int statusCode = getInstance().spHttpClient_->getStatusCode();
    if (statusCode == 200)
    {
        // 释放成功，返回选择菜单
        return;
    }
}

void ChessGame::start()
{
    menu();
}

void ChessGame::menu()
{
    // 选择人机还是联机对战
    int choice = -1;
    while (choice != 0)
    {
        displayChessGameMenu();
        std::cin >> choice;  // 选择人机对战还是联机对战
        switch(choice)
        {
           case 1: 
           {
                // 与服务端分配一个ai对战
                curGameType_ = MAN_VS_AI;
                PlayerVsAi();
                break;
            }
            case 2: 
            {
                // 跳转联机对战模式 
                curGameType_ = MAN_VS_MAN;
                // 从这里开始写
                PlayerVsPlayer();
                break;
            }
            case 0:
                std::cout << "Exiting the program. Goodbye!\n";
                break;
            default:
                std::cout << "Invalid choice! Please try again.\n";
        }
    }
}

void ChessGame::PlayerVsAi()
{
    // 封装http请求，请求与服务端机器人对战
    spHttpClient_->addJsonData("userId", userId_);
    spHttpClient_->addJsonData("gameType", curGameType_); // ai人机对战
    std::string requestBody = spHttpClient_->jsonDataToString();

    spHttpClient_->packageRequestLine(HttpMethod::kPost, "/aiBot/start");
    spHttpClient_->setContentType("application/json");
    spHttpClient_->setContentLength(requestBody.size());
    // send request
    spHttpClient_->sendRequest(requestBody);

    // receive response
    spHttpClient_->receiveResponse();
    
    // 获取响应状态码
    int statusCode = spHttpClient_->getStatusCode();
    if (statusCode == 200)
    {
        startAiBattle(); // 开始与ai对战
        return ;
    }
    else 
    {
        // 返回选择菜单
        std::cout << "Error: Failed to start the game. Please try again." << std::endl;
        return;
    }
}

void ChessGame::startAiBattle()
{
    // 开始游戏，展示棋盘
    while (true)
    {
        displayBoard();
        std::cout << "玩家(X)的回合，请输入下棋坐标(行 列，例如:1 A): ";
        int r; char c;
        std::cin >> r >> c; 
        int y = c - 'A'; int x = r - 1;  // 将棋盘棋子坐标转换为数组坐标

        if (!std::cin || x < 0 || x >= BOARD_SIZE || y < 0 || y >= BOARD_SIZE || board_[x][y] != ' ') 
        {
			std::cout << "无效输入，请重试！" << std::endl;
			std::cin.clear();
			std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
			continue;
		}

        // 更新棋盘
        board_[x][y] = 'X';
        displayBoard();
        // 在这里发送请求给服务端，服务端接收到请求后，将棋盘数组更新，然后判断用户是否胜利
        // 把下棋的坐标发送过去
        spHttpClient_->addJsonData("userId", userId_);
        spHttpClient_->addJsonData("x", x);
        spHttpClient_->addJsonData("y", y);
        std::string requestBody = spHttpClient_->jsonDataToString();

        spHttpClient_->packageRequestLine(HttpMethod::kPost, "/aiBot/move");
        spHttpClient_->setContentType("application/json");
        spHttpClient_->setContentLength(requestBody.size());
        // send request
        spHttpClient_->sendRequest(requestBody);

        // receive response
        spHttpClient_->receiveResponse();

        // 判断是何种结果
        std::string type = spHttpClient_->getJsonBody("type");
        if (std::stoi(type) == DURING_GAME)
        {
            std::string x1 = spHttpClient_->getJsonBody("x");
            std::string y1 = spHttpClient_->getJsonBody("y");
            std::cout << "AI(O)的回合，其落子坐标为: " << x1 << " " << y1 << std::endl;
            x = std::stoi(x1); y = std::stoi(y1);
            board_[x][y] = 'O';
        }
        else 
        {
            // 判断是谁胜利了
            std::string winner = spHttpClient_->getJsonBody("winner");
            if (winner == "user") // 玩家胜
            {
                displayBoard();
                std::cout << "You Win!" << std::endl;
                exit(0);
            }
            else if (winner == "ai")// ai胜
            {
                std::string x1 = spHttpClient_->getJsonBody("x");
                std::string y1 = spHttpClient_->getJsonBody("y");
                std::cout << "AI(O)的回合，其落子坐标为: " << x1 << " " << y1 << std::endl;
                x = std::stoi(x1); y = std::stoi(y1);
                board_[x][y] = 'O';
                displayBoard();
                std::cout << "You Lose!" << std::endl;
                exit(0);
            }
            else if (winner == "humanDraw")
            {
                displayBoard();
                std::cout << "平局!" << std::endl;
                exit(0);
            }
            else if (winner == "aiDraw")
            {
                std::string x1 = spHttpClient_->getJsonBody("x");
                std::string y1 = spHttpClient_->getJsonBody("y");
                std::cout << "AI(O)的回合，其落子坐标为: " << x1 << " " << y1 << std::endl;
                x = std::stoi(x1); y = std::stoi(y1);
                board_[x][y] = 'O';
                displayBoard();
                std::cout << "平局!" << std::endl;
                exit(0);
            }
            return ;
        }
    }
}

// 联机对战
void ChessGame::PlayerVsPlayer()
{
    // 封装http请求
    spHttpClient_->addJsonData("userId", userId_);
    spHttpClient_->addJsonData("gameType", curGameType_);
    std::string requestBody = spHttpClient_->jsonDataToString();

    spHttpClient_->packageRequestLine(HttpMethod::kPost, "/onlineGame/start");
    spHttpClient_->setContentType("application/json");
    spHttpClient_->setContentLength(requestBody.size());
    // send request
    spHttpClient_->sendRequest(requestBody);

    // receive response
    spHttpClient_->receiveResponse();
    
    // 获取响应状态码
    int statusCode = spHttpClient_->getStatusCode();
    if (statusCode == 200)
    {
        // 根据服务端给的响应判断是直接开始游戏还是等待对手加入

    }
}

