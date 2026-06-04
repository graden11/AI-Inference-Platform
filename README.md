# Kama-HTTPServer

基于 muduo 的高性能 C++17 AI 推理服务平台。支持 ONNX Runtime (CPU) 和 TensorRT (GPU)，多模型热加载，分类/检测/分割多任务类型。

**特性：** 事件驱动架构 · 插件式 Backend · 多模型多任务 · 热加载/热卸载 · 请求级动态批处理 · Prometheus 指标 · 结构化访问日志 · 优雅关闭 · Redis/内存双模式会话

---

## 快速开始（10 分钟）

### 前置要求

- Docker 20.10+（Compose v2）
- GPU 推理需 NVIDIA Container Toolkit
- 磁盘 ≥ 2 GB

### 1. 获取项目

```bash
git clone <repo-url> && cd httpserver
```

### 2. 放置模型文件

模型文件 **不在 Git 仓库中**（总计 ~174 MB），需放入 `WebApps/InferenceServer/models/`：

| 文件 | 大小 | 说明 |
|------|------|------|
| `resnet50_classification.onnx` | ~98 MB | ResNet-50 分类（必选） |
| `yolov8l.onnx` | ~167 MB | YOLOv8l COCO 检测（可选） |
| `resnet50_classification.engine` | ~50 MB | TensorRT FP16 引擎（GPU 可选） |
| `resnet50_int8.engine` | ~26 MB | TensorRT INT8 引擎（GPU 可选） |
| `imagenet_classes.txt` | ~10 KB | ImageNet 1000 类标签 |
| `coco_80.txt` | ~0.6 KB | COCO 80 类标签（检测用） |

> 如果模型文件缺失，`start.sh` 会给出清晰提示。无 GPU 时仅需 ONNX 模型。

### 3. 一键启动

```bash
# CPU 模式（推荐，兼容所有环境）
./start.sh cpu

# GPU 模式（需 NVIDIA GPU + CUDA）
./start.sh gpu
```

脚本自动完成：检测环境 → 编译项目 → 构建 Docker 镜像 → 启动 MySQL + Redis + 应用 → 健康检查。

### 4. 验证

```bash
curl http://localhost/health
# → {"status":"ok"}

# 查看已加载模型
curl http://localhost/models | python3 -m json.tool

# 推理测试（分类）
python3 -c "
import json,base64
with open('your_image.jpg','rb') as f:
    b64 = base64.b64encode(f.read()).decode()
json.dump({'image_data':b64,'model_name':'resnet50'}, open('/tmp/payload.json','w'))
"
curl -s -X POST http://localhost/predict -H 'Content-Type: application/json' -d @/tmp/payload.json | python3 -m json.tool
```

---

## 架构

```
客户端 (Browser / curl)
        │
        v
+----------------------------------------------------------+
|  HttpServer (muduo 事件驱动)                              |
|  +------------------+  +------------------+  +-----------+ |
|  | MetricsMiddleware|  |  CorsMiddleware  |  | Session   | |
|  +------------------+  +------------------+  +-----------+ |
|  |                   Router (16 条路由)                   | |
|  +------------------------------------------------------+ |
+----------------------------------------------------------+
        │                               │
        v                               v
+----------------------+    +--------------------------+
| InferenceServer      |    | HttpServer 框架层         |
| - ResNet50Engine     |    | - Router (精确+正则匹配)  |
| - ResNet50TRTEngine  |    | - MiddlewareChain        |
| - ModelFactory       |    | - SessionManager         |
| - RequestBatcher     |    | - DbConnectionPool       |
| - 14 个 Handler      |    | - MetricsCollector       |
+----------+-----------+    +--------------------------+
           │
           v
+------------------+
| MySQL 8.0        |  用户数据、连接池
| Redis 7          |  会话存储（可选，支持内存模式）
| ONNX Runtime     |  CPU 推理
| TensorRT 10      |  GPU 推理 (FP16/INT8)
+------------------+
```

**技术栈：** C++17 · [muduo](https://github.com/chenshuo/muduo) · MySQL Connector/C++ · spdlog · nlohmann/json · Protobuf · OpenSSL

---

## API 参考

**Base URL:** `http://localhost` · **认证:** 登录后携带 `Cookie: SESSIONID=<uuid>`

### 端点总览

| 方法 | 路径 | 认证 | 说明 |
|------|------|------|------|
| GET | `/` `/entry` | — | 登录/注册页面 |
| POST | `/login` | — | 用户登录 |
| POST | `/register` | — | 用户注册 |
| POST | `/user/logout` | 是 | 用户登出 |
| GET | `/menu` | 是 | AI 推理仪表盘 |
| GET | `/backend` | — | 管理后台 |
| GET | `/backend_data` | — | 在线统计 JSON |
| POST | `/predict` | — | 图像推理 (JSON) |
| POST | `/predict/batch` | — | 批量图像推理 |
| POST | `/predict/proto` | — | 图像推理 (Protobuf) |
| GET | `/metrics` | — | Prometheus 指标 |
| GET | `/metrics/json` | — | JSON 指标 |
| GET | `/health` | — | 存活检查 |
| GET | `/ready` | — | 就绪检查 |
| POST | `/models/load` | 是 | 动态加载模型 |
| GET | `/models` | — | 模型列表 |
| DELETE | `/models/:name/:version` | 是 | 卸载模型 |

### 核心端点示例

#### 推理 — `POST /predict`

支持分类和检测两种任务类型。`model_name` 从 `GET /models` 获取。

```bash
# 图片 base64 较大时，先写入文件再发送（避免 bash 参数上限）
python3 -c "
import json,base64
with open('/path/to/image.jpg','rb') as f:
    b64 = base64.b64encode(f.read()).decode()
json.dump({'image_data':b64,'model_name':'resnet50'}, open('/tmp/payload.json','w'))
"
curl -s -X POST http://localhost/predict \
  -H 'Content-Type: application/json' \
  -d @/tmp/payload.json | python3 -m json.tool
```

**分类响应 (resnet50):**
```json
{
  "status": "ok",
  "task_type": "classification",
  "predictions": [
    {"id": 282, "label": "tiger cat", "confidence": 27.0},
    {"id": 281, "label": "tabby", "confidence": 12.5}
  ]
}
```

**检测响应 (yolov8l):**
```json
{
  "status": "ok",
  "task_type": "detection",
  "detections": [
    {"class_id": 15, "label": "cat", "confidence": 70.8,
     "bbox": {"x1": 336, "y1": 240, "x2": 464, "y2": 368}}
  ]
}
```

**参数说明:**

| 参数 | 必填 | 说明 |
|------|------|------|
| `image_data` | 二选一 | base64 编码的图片 |
| `image_path` | 二选一 | 容器内的文件路径（`/app/models/xxx.jpg`） |
| `model_name` | 否 | 模型名，默认 `resnet50`。可从 `GET /models` 获取 |

#### 监控 — `GET /metrics`

```bash
curl http://localhost/metrics
```
输出标准 Prometheus 格式：`http_requests_total`、`http_request_duration_microseconds_bucket`、`http_requests_inflight`、`process_uptime_seconds` 等。

JSON 格式可用 `/metrics/json`。

#### 健康检查 — `GET /health` `/ready`

```bash
curl http://localhost/health
# → 200 {"status":"ok"}

curl http://localhost/ready
# → 200 {"status":"ready","checks":{"mysql":true,"redis":true,"models":true}}
# 或 503 {"status":"not_ready","checks":{"mysql":false,...}}
```

#### 模型管理

```bash
# 列出已加载的模型
curl http://localhost/models
# → [{"name":"resnet50","version":"1","type":"onnx","path":"...","is_latest":true}]

# 动态加载模型（需登录）
curl -b cookies.txt -X POST http://localhost/models/load \
  -H "Content-Type: application/json" \
  -d '{"name":"resnet50","version":"v2","type":"onnx","path":"/app/models/resnet50_v2.onnx"}'

# 卸载模型（需登录）
curl -b cookies.txt -X DELETE http://localhost/models/resnet50/v2
```

#### 用户认证

```bash
# 注册
curl -X POST http://localhost/register \
  -H "Content-Type: application/json" \
  -d '{"username":"alice","password":"123456"}'

# 登录（自动 Set-Cookie）
curl -c cookies.txt -X POST http://localhost/login \
  -H "Content-Type: application/json" \
  -d '{"username":"alice","password":"123456"}'

# 访问受保护页面
curl -b cookies.txt http://localhost/menu
```

---

## 添加新模型

### 1. 准备模型文件

将 ONNX 模型文件放入 `WebApps/InferenceServer/models/`。

### 2. 注册模型（运行时，无需重启）

```bash
# 分类模型（默认)
curl -b cookies.txt -X POST http://localhost/models/load \
  -H "Content-Type: application/json" \
  -d '{"name":"mobilenet","version":"1","type":"onnx","path":"/app/models/mobilenet.onnx"}'

# 检测模型（需指定 task）
curl -b cookies.txt -X POST http://localhost/models/load \
  -H "Content-Type: application/json" \
  -d '{"name":"yolov8l","version":"1","type":"onnx","path":"/app/models/yolov8l.onnx","task":"detection","labels":"/app/models/coco_80.txt","input_name":"images","output_name":"output0","input_width":640,"input_height":640,"input_mean":[0,0,0],"input_std":[1,1,1],"confidence_threshold":0.3}'
```

加载后自动持久化到 `config.json`，重启后自动恢复。

### 3. 测试推理

```bash
curl -X POST http://localhost/predict \
  -H "Content-Type: application/json" \
  -d '{"image_path":"/app/models/cat.jpg","model_name":"mobilenet"}'
```

### 4. （可选）添加到静态配置

编辑 `config.json` → `models.engines`：
```json
"mobilenet": {
  "type": "onnx",
  "path": "models/mobilenet.onnx"
}
```

静态配置的模型在启动时加载，**不可**通过 API 卸载。

> 自定义推理引擎（非 ResNet-50 架构）需实现 `InferenceEngine` 接口并注册到 `ModelFactory`，详见 [LEARNING.md](LEARNING.md)。

---

## 配置

### config.json

```json
{
  "server": { "port": 80, "threads": 4, "log_level": "WARN", "shutdown_timeout_ms": 30000 },
  "logging": { "level": "INFO", "file": "server.log", "access_log": "access.log" },
  "mysql": { "host": "tcp://mysql:3306", "user": "root", "password": "root", "database": "inference_platform", "pool_size": 10 },
  "redis": { "host": "redis", "port": 6379, "pool_size": 5 },
  "models": { "labels_path": "/app/models/imagenet_classes.txt", "engines": { "resnet50": { "type": "onnx", "path": "/app/models/resnet50_classification.onnx" } } },
  "batching": { "enabled": false, "max_batch_size": 8, "max_delay_ms": 10 }
}
```

### 命令行参数

| 参数 | 说明 | 示例 |
|------|------|------|
| `-c <path>` | 配置文件 | `-c config.json` |
| `-p <port>` | 覆盖端口 | `-p 8080` |
| `-t <n>` | I/O 线程数 | `-t 8` |
| `-l <level>` | 日志级别 | `-l DEBUG` |

CLI 参数优先级高于配置文件。

---

## 部署

### Docker Compose（推荐）

```bash
# CPU 部署（阿里云/腾讯云 2核4G）
./start.sh cpu

# GPU 部署（NVIDIA GPU + CUDA 12.6）
./start.sh gpu
```

### 手动部署

```bash
mkdir -p build && cd build
cmake .. -DENABLE_TENSORRT=OFF     # CPU: OFF, GPU: ON
make -j$(nproc)
cd ..

# CPU
docker compose -f docker-compose.cpu.yml up -d

# GPU
docker compose up -d
```

### 阿里云 ECS 注意

```bash
# 上传项目（排除 build/）
rsync -avz --exclude 'build/' ./ root@<服务器IP>:~/httpserver/
# 在控制台防火墙中放行 TCP 80 端口
# 服务器上运行：
cd ~/httpserver && ./start.sh cpu
```

### 常用运维命令

```bash
docker compose logs -f httpserver      # 查看日志
docker compose down && ./start.sh cpu  # 重建重启
cat access.log | jq .                  # 查看结构化访问日志
curl http://localhost/metrics          # Prometheus 指标
```

---

## 性能

硬件：NVIDIA RTX 5060 (8 GB), 16 核 CPU, Ubuntu 22.04

| 模型 | 延迟 (avg) | P99 | QPS |
|------|-----------|-----|-----|
| TensorRT INT8 | 13.8 ms | 15.2 ms | 142 |
| TensorRT FP16 | 16.3 ms | 20.0 ms | 152 |
| ONNX CPU | 76.3 ms | 83.6 ms | 44 |

详见 [pressureresult.md](pressureresult.md)。

---

## 开发

### 快速开始

```bash
# 使用开发容器
docker build -f Dockerfile.dev -t kama-httpserver:dev .
docker run -it --gpus all -v "$(pwd):/project" -p 80:80 kama-httpserver:dev
# 容器内：cmake .. && make -j$(nproc) && ./simple_server -c ../WebApps/InferenceServer/config.json
```

### 文档索引

| 文档 | 内容 |
|------|------|
| [CLAUDE.md](CLAUDE.md) | 构建命令、架构细节、AI 助手使用指南 |
| [LEARNING.md](LEARNING.md) | 9 阶段学习路线、设计模式速查 |
| [DEVELOPMENT.md](DEVELOPMENT.md) | 开发方向、技术选型讨论 |
| [DEVLOG.md](DEVLOG.md) | Claude Code 开发日志 |

---

## 已知限制

- **Linux only**：muduo 基于 epoll，不支持 Windows/macOS
- **模型文件不在仓库**：~174 MB，需单独放置
- **GPU 推理串行**：`gpu_mutex_` 同一时刻一个 GPU 任务
- **无频率限制**：公网暴露需额外保护
- **项目路径依赖**：HTML 资源通过 CWD 相对路径读取

---

## 致谢

[muduo](https://github.com/chenshuo/muduo) · [ONNX Runtime](https://github.com/microsoft/onnxruntime) · [TensorRT](https://developer.nvidia.com/tensorrt) · [spdlog](https://github.com/gabime/spdlog) · [nlohmann/json](https://github.com/nlohmann/json) · [stb](https://github.com/nothings/stb)
