#pragma once

#include <unistd.h> // usleep
#include <locale.h>
#include <wchar.h>

#include <iostream>
#include <limits>
#include <memory>
#include <sstream>


#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/StreamCopier.h>

#include <ncurses.h>

#include "ChessGame.h"
#include "HttpClient.h"

class GomokuClient
{
public:
    GomokuClient(const std::string& host, int port);

    void start()
    {
        menu();
    }

private:
    // 下一步写选择菜单栏，选择人机对战还是联机对战

    // 菜单界面
    void menu();

    // 登录
    void login();

    void registerUser();

    void clearScreen() 
    {
        #if defined(_WIN32) || defined(_WIN64)
            system("cls");
        #else
            system("clear");
        #endif
    }

    void displayMainMenu() 
    {
        clearScreen();
        std::cout << "===================================\n";
        std::cout << "          Welcome to Gomoku\n";
        std::cout << "===================================\n";
        std::cout << "[1] Login\n";
        std::cout << "[2] Register\n";
        std::cout << "[0] Exit\n";
        std::cout << "===================================\n";
        std::cout << "Pressing other option will directly exit the program!\n";
        std::cout << "Please choose an option: ";
    }

    void displayLoginMenu() 
    {
        clearScreen();
        std::cout << "===================================\n";
        std::cout << "             LOGIN\n";
        std::cout << "===================================\n";
        std::cout << "Username: ________________________\n";
        std::cout << "Password: ________________________\n";
        std::cout << "[0] Back to Main Menu\n";
        std::cout << "===================================\n";
    }

    void displayRegisterMenu() 
    {
        clearScreen();
        std::cout << "===================================\n";
        std::cout << "            REGISTER\n";
        std::cout << "===================================\n";
        std::cout << "New Username: ____________________\n";
        std::cout << "New Password: ____________________\n";
        std::cout << "Confirm Password: ________________\n";
        std::cout << "[0] Back to Main Menu\n";
        std::cout << "===================================\n";
    }

    // 设置光标到指定位置
    void gotoxy(int x, int y) 
    {
        move(y, x);
        refresh();
    }

    // 加载界面函数
    void gotoc() 
    {
        initscr();           // 初始化 ncurses
        // setlocale(LC_ALL, ""); // 启用区域支持（UTF-8）
        std::ios::sync_with_stdio(false);
        std::locale::global(std::locale("en_US.UTF-8"));
        std::wcout.imbue(std::locale());
        noecho();            // 禁止显示输入字符
        curs_set(0);         // 隐藏光标

        clear();             // 清屏
        int startX = (COLS - 20) / 2; 
        gotoxy(startX, 5);
        //mvaddwstr(25, 5, L"卡 码 五 子 棋"); // 使用宽字符输出中文
        // printw("卡 码 五 子 棋"); // 中文标题
        std::cout << "卡 码 五 子 棋" << std::endl;

        startX = (COLS - 13) / 2; 
        gotoxy(startX, 10);
        std::cout << "加载中..." << std::endl;

        startX = (COLS - 25) / 2;
        gotoxy(startX, 11);
        std::cout << "作者：代码随想录" << std::endl;

        // 动态显示进度条
        for (int j = 0; j <= 100; j++) 
        {
            usleep(50000); // 延时
            startX = (COLS - 18) / 2;
            gotoxy(startX, 8);
            printw("loading: %d%%", j); // 中文进度
            startX = (COLS - 100) / 2;
            gotoxy(startX + j, 8); // 动态位置
            printw("#");
            refresh();
        }
        clear(); // 清屏

        endwin();       // 恢复终端
    }       


private:
    // 基于httpClient进行在封装
    Poco::JSON::Object           jsonObj_;
    std::unique_ptr<ChessGame>   chessGame_;
    std::shared_ptr<HttpClient>  httpClient_;  // 对http相关进行封装
};

        