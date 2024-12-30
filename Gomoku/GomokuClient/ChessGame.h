#pragma once

#include <csignal> // 用于捕获信号
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "HttpClient.h"

const int BOARD_SIZE = 13;

#define DURING_GAME 1
#define GAME_OVER 2

const char EMPTY = ' ';
const char HUMAN_PLAYER = 'X'; // 人类玩家棋子符号
const char AI_PLAYER = 'O'; // AI玩家棋子符号

class ChessGame
{
public:
    
    // 获取单例
    static ChessGame& getInstance()
    {
        static ChessGame instance;
        return instance;
    }
    
    // 初始化五子棋游戏
    void init(int uesrId, std::shared_ptr<HttpClient> spHttpClient);

    // 开启五子棋游戏
    void start();

    const int getUserId() const
    {
        return userId_;
    }

    const int getCurGameType() const
    {
        return curGameType_;
    }

private:
    ChessGame()
        : board_(BOARD_SIZE, std::vector<char>(BOARD_SIZE, ' '))
    {}

    void menu();
    // 玩家对决ai
    void PlayerVsAi();
    void startAiBattle();
    // 玩家对战玩家
    void PlayerVsPlayer();

    static void signalHandler(int signum);
    static void sendReleaseRequest();

    // 清屏
    void clearScreen() 
    {
        #if defined(_WIN32) || defined(_WIN64)
            system("cls");
        #else
            system("clear");
        #endif
    }
    
    void displayChessGameMenu() 
    {
        clearScreen();
        std::cout << "===================================\n";
        std::cout << "         欢迎来到卡码五子棋\n";
        std::cout << "===================================\n";
        std::cout << "[1] 对战ai\n";
        std::cout << "[2] 匹配玩家\n";
        std::cout << "[0] 退出游戏\n";
        std::cout << "===================================\n";
        std::cout << "Please choose an option: ";
    }

    void displayBoard() 
    {
        clearScreen();
        std::cout << "    "; // 列标题
        for (char c = 'A'; c < 'A' + BOARD_SIZE; c++) 
        {
            std::cout << c << "   ";
        }
        std::cout << std::endl;
        // 一开始隔开五个位置，后续其实也是五个位置加上字符的话

        for (int r = 0; r < BOARD_SIZE; r++) 
        {
            std::cout << r + 1 << (r < 9 ? "   " : "  "); // 行标题，这里同样是确保占五个位置
            for (int c = 0; c < BOARD_SIZE; c++) 
            {
                std::cout << board_[r][c]; // 该这个数组就行那确实简单很多，数组是跟棋盘图像是分开的爽飞了，到时判断是否赢也是一个逻辑
                if (c < BOARD_SIZE - 1) std::cout << " | ";
            }
            std::cout << std::endl;
            if (r < BOARD_SIZE - 1) 
            {
                std::cout << "   ";
                for (int c = 0; c < BOARD_SIZE; c++) 
                {
                    std::cout << "---";
                    if (c < BOARD_SIZE - 1) std::cout << "-";
                }
                std::cout << std::endl;
            }
        }
    }


private:
    enum GameType
    {
        NO_GAME = 0,
        MAN_VS_AI = 1,
        MAN_VS_MAN = 2
    };
    
    int                            userId_;
    int                            curGameType_;
    std::shared_ptr<HttpClient>    spHttpClient_;
    std::vector<std::vector<char>> board_; // 通信双方各维护一个棋盘需要保证数据一致性
};