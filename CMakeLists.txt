cmake_minimum_required(VERSION 3.10)
project(WebServer VERSION 1.0)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 设置编译选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")

# 查找必需的包
find_package(Threads REQUIRED)  # pthread

# 设置 muduo 库路径（根据实际安装路径调整）
set(MUDUO_PATH "/usr/local")  # muduo 安装路径
include_directories(${MUDUO_PATH}/include)
link_directories(${MUDUO_PATH}/lib)

# 设置 MySQL Connector/C++ 路径（根据实际安装路径调整）
set(MYSQL_CONNECTOR_PATH "/usr/local")  # MySQL Connector 安装路径
include_directories(${MYSQL_CONNECTOR_PATH}/include)
link_directories(${MYSQL_CONNECTOR_PATH}/lib)

# 查找所有源文件
file(GLOB_RECURSE HTTP_SERVER_SOURCES
    "${PROJECT_SOURCE_DIR}/HttpServer/src/*.cpp"
    "${PROJECT_SOURCE_DIR}/HttpServer/include/*.cpp"
)

file(GLOB_RECURSE GOMOKU_SOURCES
    "${PROJECT_SOURCE_DIR}/WebApps/GomokuServer/src/*.cpp"
    "${PROJECT_SOURCE_DIR}/WebApps/GomokuServer/include/*.cpp"
)

# 设置所有源文件
set(ALL_SOURCES
    ${PROJECT_SOURCE_DIR}/main.cpp
    ${HTTP_SERVER_SOURCES}
    ${GOMOKU_SOURCES}
)

# 包含头文件目录
include_directories(
    ${PROJECT_SOURCE_DIR}/HttpServer/include
    ${PROJECT_SOURCE_DIR}/WebApps/GomokuServer/include
)

# 添加可执行文件
add_executable(webserver ${ALL_SOURCES})

# 链接必需的库
target_link_libraries(webserver
    PRIVATE 
    Threads::Threads     # pthread
    muduo_net           # muduo 网络库
    muduo_base          # muduo 基础库
    mysqlcppconn       # MySQL Connector/C++
)

# 添加示例服务器
add_executable(simple_server 
    ${PROJECT_SOURCE_DIR}/HttpServer/examples/simpleServer.cpp
    ${HTTP_SERVER_SOURCES}
)

target_link_libraries(simple_server
    PRIVATE 
    Threads::Threads
    muduo_net
    muduo_base
)