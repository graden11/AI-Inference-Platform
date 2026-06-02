cmake_minimum_required(VERSION 3.12)
project(simple_server)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译选项：GPU 推理（TensorRT），云服务器部署时设为 OFF
option(ENABLE_TENSORRT "Build with TensorRT GPU inference" ON)

# 在文件开头添加 OpenSSL 查找
find_package(OpenSSL REQUIRED)

# 添加头文件路径
include_directories(
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/HttpServer/include
    ${PROJECT_SOURCE_DIR}/WebApps/InferenceServer/include
    /usr/include/mysql-cppconn-8
    /usr/include/mysql
    ${PROJECT_SOURCE_DIR}/third_party/onnxruntime/include
    ${PROJECT_SOURCE_DIR}/third_party/stb
    ${PROJECT_SOURCE_DIR}/third_party/muduo
    /usr/include/spdlog
    ${CMAKE_BINARY_DIR}/generated
    ${OPENSSL_INCLUDE_DIR}
)

if(ENABLE_TENSORRT)
    include_directories(/usr/local/cuda/include)
    add_definitions(-DENABLE_TENSORRT)
endif()

# 查找必要的库
find_library(MYSQLCPPCONN_LIBRARY
    NAMES mysqlcppconn mysqlcppconn8
    PATHS /usr/lib /usr/lib64 /usr/local/lib
)

find_library(HIREDIS_LIBRARY
    NAMES hiredis
    PATHS /usr/lib /usr/lib64 /usr/local/lib
)

if(HIREDIS_LIBRARY)
    add_definitions(-DENABLE_REDIS)
    message(STATUS "Redis support: ENABLED (hiredis found)")
else()
    message(STATUS "Redis support: DISABLED (hiredis not found, using in-memory only)")
endif()

# --- Protobuf 代码生成 ---
set(PROTO_DIR "${PROJECT_SOURCE_DIR}/proto")
set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${GENERATED_DIR})

add_custom_command(
    OUTPUT ${GENERATED_DIR}/inference.pb.cc ${GENERATED_DIR}/inference.pb.h
    COMMAND protoc
        --proto_path=${PROTO_DIR}
        --cpp_out=${GENERATED_DIR}
        ${PROTO_DIR}/inference.proto
    DEPENDS ${PROTO_DIR}/inference.proto
    COMMENT "Generating protobuf from inference.proto"
)

# 添加所有源文件
file(GLOB_RECURSE HTTP_SERVER_SRC
    CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/HttpServer/src/*.cpp"
)

file(GLOB_RECURSE INFERENCE_SERVER_SRC
    CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/WebApps/InferenceServer/src/*.cpp"
)

if(NOT ENABLE_TENSORRT)
    list(FILTER INFERENCE_SERVER_SRC EXCLUDE REGEX "ResNet50TRTEngine\\.cpp")
endif()

if(NOT HIREDIS_LIBRARY)
    list(FILTER HTTP_SERVER_SRC EXCLUDE REGEX "RedisSessionStorage\\.cpp")
endif()

# 添加主程序文件
set(MAIN_SRC "${PROJECT_SOURCE_DIR}/WebApps/InferenceServer/src/main.cpp")

# 如果主程序文件不存在，创建一个
if(NOT EXISTS ${MAIN_SRC})
    file(WRITE ${MAIN_SRC}
"#include \"../include/InferenceServer.h\"
#include <iostream>

int main(int argc, char* argv[]) {
    try {
        InferenceServer server;
        server.start();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << \"Error: \" << e.what() << std::endl;
        return 1;
    }
}
")
endif()

# 添加可执行文件
add_executable(simple_server
    ${MAIN_SRC}
    ${HTTP_SERVER_SRC}
    ${INFERENCE_SERVER_SRC}
    ${GENERATED_DIR}/inference.pb.cc
)

# 链接必要的库
target_link_libraries(simple_server
    pthread
    ${PROJECT_SOURCE_DIR}/third_party/muduo/lib/libmuduo_net.a
    ${PROJECT_SOURCE_DIR}/third_party/muduo/lib/libmuduo_base.a
    mysqlcppconn
    mysqlclient
    ssl
    crypto
    crypt
    protobuf
    ${PROJECT_SOURCE_DIR}/third_party/onnxruntime/lib/libonnxruntime.so
    /usr/lib/x86_64-linux-gnu/libfmt.so
)

if(HIREDIS_LIBRARY)
    target_link_libraries(simple_server ${HIREDIS_LIBRARY})
endif()

if(ENABLE_TENSORRT)
    target_link_libraries(simple_server
        /usr/lib/x86_64-linux-gnu/libnvinfer.so
        /usr/lib/x86_64-linux-gnu/libnvonnxparser.so
        /usr/local/cuda/lib64/libcudart.so
    )
endif()

# 打印调试信息
message(STATUS "Include directories:")
get_property(dirs DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY INCLUDE_DIRECTORIES)
foreach(dir ${dirs})
    message(STATUS "  ${dir}")
endforeach()

message(STATUS "Main source file: ${MAIN_SRC}")
message(STATUS "HTTP Server sources: ${HTTP_SERVER_SRC}")
message(STATUS "Inference Server sources: ${INFERENCE_SERVER_SRC}")