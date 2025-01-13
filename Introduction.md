# HTTP Server Framework

## 项目目录结构

HttpServer/
├── include/
│   ├── http/
│   │   ├── HttpContext.h
│   │   ├── HttpRequest.h
│   │   ├── HttpResponse.h
│   │   └── HttpServer.h
│   │   
│   ├── router/
│   │   ├── Router.h
│   │   └── RouterHandler.h
│   ├── middleware/
|   |   ├── MiddlewareChain.h
|   |   ├── Middleware.h
|   |   └── cors/
|   |       ├── CorsConfig.h
|   |       └── CorsMiddleware.h
│   ├── session/                # 新增：Session 相关头文件
│   │   ├── Session.h
│   │   ├── SessionManager.h
│   │   └── SessionStorage.h
│   └── utils/
│       ├── FileUtil.h
│       ├── JsonUtil.h
│       ├── MysqlUtil.h
│       └── db/
│           ├── DbConnection.h
│           ├── DbConnectionPool.h
│           └── DbException.h
├── src/
│   ├── http/
│   │   ├── HttpContext.cpp
│   │   ├── HttpRequest.cpp
│   │   ├── HttpResponse.cpp
│   │   └── HttpServer.cpp
│   ├── router/
│   │   └── Router.cpp
│   ├── middleware/
│   │   ├── MiddlewareChain.cpp
│   │   └── cors/
│   │       └── CorsMiddleware.cpp
│   ├── session/                # 新增：Session 相关实现
│   │   ├── Session.cpp
│   │   ├── SessionManager.cpp
│   │   └── SessionStorage.cpp
│   └── utils/
│       ├── FileUtil.cpp
│       └── db/
│           ├── DbConnection.cpp
│           └── DbConnectionPool.cpp
└── tests/
    └── HttpServerTest.cpp



