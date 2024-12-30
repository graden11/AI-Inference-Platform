#pragma once

#include <memory>

#include "../HttpClient.h"

class BackgroundManager
{
public:
    BackgroundManager()
        : httpClient_("127.0.0.1", 8888)
    {
    }

    void start()
    {   // 我这个应该做成实时的
        showServerUsersData();
    }

    void showServerUsersData()
    {
        httpClient_.packageRequestLine(HttpMethod::kGet, "/serverData");
        httpClient_.setContentType("application/json");
        // send request
        httpClient_.sendRequest();

        // receive response
        httpClient_.receiveResponse();


    }

private:
    HttpClient httpClient_;
};

int main()
{
    // 查看展示当前用户在线人数、历史最高人数

}