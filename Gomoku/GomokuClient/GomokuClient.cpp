#include "GomokuClinet.h"

GomokuClient::GomokuClient(const std::string& host, int port)
        : httpClient_(std::make_shared<HttpClient>(host, port))
{
    
}

void GomokuClient::menu()
{
    // 展示菜单界面
    int choice = -1;
    while (choice != 0)
    {
        displayMainMenu();
        std::cin >> choice;
        switch(choice)
        {
            case 1: 
            {
                // 登录逻辑
                login();
                break;
            }
            case 2: 
            {
                registerUser();
                break;
            }
            case 0:
            {
                std::cout << "Exiting the program. Goodbye!\n";
                break;
            }
        }
    }
}

void GomokuClient::login()
{
    // 展示登录界面
    displayLoginMenu();
    std::string username, password;
    std::cout << "Enter username: ";
    std::cin >> username;
    std::cout << "Enter password: ";
    std::cin >> password;
    
    // 封装请求
    // http request
    httpClient_->addJsonData("username", username);
    httpClient_->addJsonData("password", password);
    std::string requestBody = httpClient_->jsonDataToString();
    
    httpClient_->packageRequestLine(HttpMethod::kPost, "/login");
    httpClient_->setContentType("application/json");
    httpClient_->setContentLength(requestBody.size());
    // send request
    httpClient_->sendRequest(requestBody);
    
    // 等待接收响应
    // Receive response
    httpClient_->receiveResponse();
    
    // 获取状态码
    int statusCode = httpClient_->getStatusCode();
    if (statusCode == 200)
    {
        std::cout << "Login success!" << std::endl;
        // 这里往后写游戏加载界面
        gotoc();
        // 创建ChessGame对象，然后进入游戏界面选择对战人机还是联机对战
        // 登录之后返回userId
        std::string userId = httpClient_->getJsonBody("userId");
        // chessGame_ = std::make_unique<ChessGame>(std::stoi(userId), httpClient_);
        // chessGame_->start();
        ChessGame::getInstance().init(std::stoi(userId), httpClient_);
        ChessGame::getInstance().start();
    }
    else if (statusCode == 401)
    {
        std::cout << "账号或密码错误, 请重新输入!" << std::endl;
        std::cout << "Press [1] to re-enter, press [0] to return to menu." << std::endl;
        int choice = -1;
        std::cin >> choice;
        while (std::cin.fail() || (choice != 0 && choice != 1))
        {
            // 输入非法，导致输入流进入错误状态，需要清空输入流
            if (std::cin.fail())
            {
                // 清除错误状态
                std::cin.clear();
                // 跳过非法输入，忽略直到遇到换行符
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            }

            std::cout << "\n非法输入! 按[1]表示重新输入，按[0]表示返回菜单.\n";
            std::cout << "请重试: ";
            std::cin >> choice;
        }

        if (choice == 1)
            login();
    }
    else if (statusCode == 403)
    {
        std::cout << "登录失败, 该账号正在其他地方登录中!" << std::endl;
        std::cout << "Press [1] to re-enter, press [0] to return to menu." << std::endl;
        int choice = -1;
        std::cin >> choice;
        while (std::cin.fail() || (choice != 0 && choice != 1))
        {
            // 输入非法，导致输入流进入错误状态，需要清空输入流
            if (std::cin.fail())
            {
                // 清除错误状态
                std::cin.clear();
                // 跳过非法输入，忽略直到遇到换行符
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            }

            std::cout << "\n非法输入! 按[1]表示重新输入，按[0]表示返回菜单.\n";
            std::cout << "请重试: ";
            std::cin >> choice;
        }

        if (choice == 1)
            login();
    }
}

void GomokuClient::registerUser()
{
    displayRegisterMenu();
    std::string username, password, confirmPassword;
    std::cout << "Enter new username: ";
    std::cin >> username;
    std::cout << "Enter new password: ";
    std::cin >> password;
    std::cout << "Confirm password: ";
    std::cin >> confirmPassword;

    if (password == confirmPassword) 
    {
        httpClient_->addJsonData("username", username);
        httpClient_->addJsonData("password", password);
        std::string requestBody = httpClient_->jsonDataToString();

        httpClient_->packageRequestLine(HttpMethod::kPost, "/register");
        httpClient_->setContentType("application/json");
        httpClient_->setContentLength(requestBody.size());
        // send request
        httpClient_->sendRequest(requestBody);
        
        // Receive response
        httpClient_->receiveResponse();

        // 获取状态码
        int statusCode = httpClient_->getStatusCode();
        if (statusCode == 200)
        {
            std::cout << "Registration successful for " << username << "!" << std::endl;
            std::cout << "Press any key to continue..." << std::endl;
            std::cin.get(); // 等待用户输入一个字符
            login();
        }
        else
        {
            std::cout << "username is exist, please re-enter!" << std::endl;
            // 选择继续注册还是菜单
            std::cout << "Press [1] to re-enter, press [0] to return to menu." << std::endl;
            int choice = -1;
            std::cin >> choice;
            while (std::cin.fail() || (choice != 0 && choice != 1))
            {
                // 输入非法，导致输入流进入错误状态，需要清空输入流
                if (std::cin.fail())
                {
                    // 清除错误状态
                    std::cin.clear();
                    // 跳过非法输入，忽略直到遇到换行符
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                }

                std::cout << "\n非法输入! 按[1]表示重新输入，按[0]表示返回菜单.\n";
                std::cout << "请重试: ";
                std::cin >> choice;
            }
            
            if (choice == 1)
                registerUser();
        }
    }
    else // 前后两次密码不匹配
    {
        std::cout << "Passwords do not match. Try again.\n";
        std::cout << "Press [1] to re-enter, press [0] to return to menu.\n";
        int choice = -1;
        std::cin >> choice;
        // 判断输入是否合法
        while (std::cin.fail() || (choice != 0 && choice != 1))
        {
            // 输入非法，导致输入流进入错误状态，需要清空输入流
            if (std::cin.fail())
            {
                // 清除错误状态
                std::cin.clear();
                // 跳过非法输入，忽略直到遇到换行符
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            }

            std::cout << "\n非法输入! 按[1]表示重新输入，按[0]表示返回菜单.\n";
            std::cout << "请重试: ";
            std::cin >> choice;
        }

        if (choice == 1)
            registerUser();
    }
}