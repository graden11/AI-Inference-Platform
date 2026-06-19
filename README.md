# AI Inference Platform

基于 muduo 的高性能 C++17 AI 推理服务平台。支持 ONNX Runtime (CPU) 和 TensorRT (GPU) 双后端，9 个模型覆盖分类/检测/特征提取 3 种任务，运行时热加载/热卸载，动态批处理，自适应硬件配置。

**特性：** 事件驱动架构 · ONNX + TensorRT 双后端 · 分类/检测/特征提取 · 热加载/热卸载 · 请求级动态批处理 · 自适应硬件配置 · 速率限制 · Prometheus 指标 · 结构化访问日志 · 优雅关闭 · Redis/内存双模式会话

---

## 快速开始（10 分钟）

### 前置要求

- Docker 20.10+（Compose v2）
- GPU 推理需 NVIDIA Container Toolkit
- 磁盘 ≥ 2 GB

### 1. 获取项目

```bash
git clone https://github.com/graden11/AI-Inference-Platform.git && cd httpserver
```

### 2. 放置模型文件

模型文件 **不在 Git 仓库中**，需放入 `models/`：

| 文件 | 大小 | 类型 | 任务 |
|------|------|------|------|
| `resnet50_classification.onnx` | ~97 MB | ONNX | 分类 |
| `squeezenet1.1-7.onnx` | ~5 MB | ONNX | 分类 |
| `yolov8l.onnx` | ~175 MB | ONNX | 检测 |
| `vision_model.onnx` | ~143 MB | ONNX | 特征提取 |
| `resnet50_classification.engine` | ~52 MB | TensorRT FP16 | 分类 |
| `squeezenet1.1-7.engine` | ~5 MB | TensorRT FP16 | 分类 |
| `yolov8l.engine` | ~97 MB | TensorRT FP16 | 检测 |
| `vision_model.engine` | ~95 MB | TensorRT FP16 | 特征提取 |
| `imagenet_classes.txt` | ~10 KB | — | ImageNet 1000 类标签 |

> ONNX 模型合计 ~420 MB。TensorRT engine 文件 ~250 MB，仅 GPU 部署需要。模型路径前缀（`../WebApps/InferenceServer/models/`→`models/`）在 Docker 构建时自动修正。

### 3. 一键启动

```bash
# CPU 模式（推荐，兼容所有环境）
./start.sh cpu

# GPU 模式（需 NVIDIA GPU + CUDA 12.6）
./start.sh gpu
```

脚本自动完成：检测环境 → 编译项目 → 构建 Docker 镜像 → 启动 MySQL + Redis + 应用 → 健康检查。

### 4. 验证

```bash
curl http://localhost/health
# → {"status":"ok"}

# 查看已加载模型
curl http://localhost/models | python3 -m json.tool

# 批量推理（两图 base64，推荐方式）
python3 -c "
import json,base64
img1 = base64.b64encode(open('image1.jpg','rb').read()).decode()
img2 = base64.b64encode(open('image2.jpg','rb').read()).decode()
json.dump({'model_name':'squeezenet1.1-7_trt','images':[img1,img2]}, open('/tmp/payload.json','w'))
"
curl -s -X POST http://localhost/predict/batch -H 'Content-Type: application/json' -d @/tmp/payload.json
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
|  |                   Router (20+ 条路由)                  | |
|  +------------------------------------------------------+ |
+----------------------------------------------------------+
        │                               │
        v                               v
+----------------------+    +--------------------------+
| InferenceServer      |    | HttpServer 框架层         |
| - OnnxBackend        |    | - Router (精确+正则匹配)  |
| - TRTBackend         |    | - MiddlewareChain        |
| - ModelFactory       |    | - SessionManager         |
| - ModelPipeline      |    | - DbConnectionPool       |
| - DynamicBatchSchd.  |    | - MetricsCollector       |
| - RequestSlotPool    |    +--------------------------+
| - ThreadPool         |
| - HardwareDetector   |
| - ConfigAdvisor      |
| - 18 个 Handler      |
+----------+-----------+    +--------------------------+
           │
           v
+------------------+
| MySQL 8.0        |  用户数据、连接池
| Redis 7          |  会话存储（可选，支持内存模式）
| ONNX Runtime     |  CPU 推理
| TensorRT 10      |  GPU 推理 (FP16)
+------------------+
```

**技术栈：** C++17 · [muduo](https://github.com/chenshuo/muduo) · MySQL Connector/C++ · spdlog · nlohmann/json · Protobuf · OpenSSL

---

## API 参考

**Base URL:** `http://localhost` · **认证:** 登录后携带 `Cookie: sessionId=<uuid>` · **Content-Type:** `application/json`（除 `/predict/raw`）

### 端点总览

| 方法 | 路径 | 认证 | 说明 |
|------|------|------|------|
| GET | `/` `/entry` | — | 登录/注册页面 |
| POST | `/login` | — | 用户登录 |
| POST | `/register` | — | 用户注册 |
| POST | `/user/logout` | 是 | 用户登出 |
| GET | `/menu` | 是 | AI 推理仪表盘 |
| GET | `/backend` | 是 | 管理后台 |
| GET | `/backend_data` | 是 | 在线统计 JSON |
| POST | `/predict` | — | 单图推理 (JSON) |
| POST | `/predict/batch` | — | 批量图像推理 (JSON) |
| POST | `/predict/raw` | — | 原始图像推理 (binary body) |
| POST | `/predict/proto` | — | 图像推理 (Protobuf) |
| POST | `/models/load` | 是 | 动态加载模型 |
| DELETE | `/models/:name/:version` | 是 | 卸载模型 |
| GET | `/models` | — | 模型列表 |
| GET | `/models/available` | 是 | 列出 models 目录文件 |
| GET | `/models/labels` | 是 | 列出标签文件 |
| POST | `/models/delete` | 是 | 删除模型文件 |
| POST | `/models/convert` | 是 | ONNX → TensorRT 转换 (GPU only) |
| GET | `/models/convert/status` | 是 | 查看转换进度 |
| GET | `/metrics` | — | Prometheus 指标 |
| GET | `/metrics/json` | — | JSON 指标 |
| GET | `/health` | — | 存活检查 |
| GET | `/ready` | — | 就绪检查 |
| GET | `/system/hardware` | — | 硬件配置 + 推荐 profile |
| GET | `/system` | — | 系统配置管理页面 |
| POST | `/system/config/apply` | 是 | 应用 stable / aggressive 配置 |
| POST | `/system/restart` | 是 | 触发优雅重启 |

### 核心端点示例

#### 推理 — `POST /predict` / `POST /predict/batch`

单图推理和批量推理共享模型及参数格式。批量推理自动并行预处理，推荐用于多图场景。

**请求 (batch):**
```json
{
  "model_name": "squeezenet1.1-7_trt",
  "images": ["<base64_img1>", "<base64_img2>"]
}
```

**请求 (单图):**
```json
{
  "model_name": "resnet50_classification_onnx",
  "image_data": "<base64>"
}
```

或使用文件路径（容器内）：
```json
{
  "model_name": "yolov8l_onnx",
  "image_path": "/app/models/cat.jpg"
}
```

```bash
# 批量推理（推荐：base64 较大时先写文件再发送）
python3 -c "
import json,base64
img1 = base64.b64encode(open('img1.jpg','rb').read()).decode()
img2 = base64.b64encode(open('img2.jpg','rb').read()).decode()
json.dump({'model_name':'squeezenet1.1-7_trt','images':[img1,img2]}, open('/tmp/payload.json','w'))
"
curl -s -X POST http://localhost/predict/batch \
  -H 'Content-Type: application/json' \
  -d @/tmp/payload.json | python3 -m json.tool
```

**分类响应:**
```json
{
  "status": "ok",
  "model_name": "squeezenet1.1-7_trt",
  "count": 2,
  "results": [
    {
      "status": "ok",
      "task_type": "classification",
      "summary": "识别结果：table lamp（1.9%），其他可能：ballpoint（1.8%）…",
      "predictions": [
        {"id": 846, "label": "table lamp", "confidence": 1.9},
        {"id": 418, "label": "ballpoint", "confidence": 1.8}
      ]
    }
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
| `image_data` / `image_path` | `predict` 二选一 | base64 字符串或容器内文件路径 |
| `images` / `image_paths` | `predict/batch` 二选一 | base64 数组或路径数组 |
| `model_name` | 否 | 模型名，默认 `resnet50`。从 `GET /models` 获取 |

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
# → [{"name":"resnet50_classification_trt","version":"1","type":"tensorrt","is_latest":true}]

# 动态加载模型（需登录）
curl -b cookies.txt -X POST http://localhost/models/load \
  -H "Content-Type: application/json" \
  -d '{"name":"resnet50","version":"v2","type":"onnx","path":"/app/models/resnet50_v2.onnx"}'

# 卸载模型（需登录）
curl -b cookies.txt -X DELETE http://localhost/models/resnet50/v2

# 删除模型文件（需登录）
curl -b cookies.txt -X POST http://localhost/models/delete \
  -H "Content-Type: application/json" \
  -d '{"path":"/app/models/old_model.onnx"}'
```

#### 自适应硬件配置

```bash
# 查看当前配置和推荐 profile
curl http://localhost/system/hardware

# 应用性能模式（需登录，重启后生效）
curl -b cookies.txt -X POST http://localhost/system/config/apply \
  -H "Content-Type: application/json" \
  -d '{"profile":"aggressive"}'

# 触发重启使配置生效
curl -b cookies.txt -X POST http://localhost/system/restart -d '{}'

# 恢复稳定模式
curl -b cookies.txt -X POST http://localhost/system/config/apply \
  -d '{"profile":"stable"}'
curl -b cookies.txt -X POST http://localhost/system/restart -d '{}'
```

配置切换会写回 bind-mounted 源文件，容器重启后保持。`stable` 保留系统冗余（batch=16, threads=8, rate=1000/s），`aggressive` 最大化吞吐（batch=64, threads=12, 无速率限制）。

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

将 ONNX 模型文件放入 `models/`。

### 2. 注册模型（运行时，无需重启）

```bash
# 分类模型（默认）
curl -b cookies.txt -X POST http://localhost/models/load \
  -H "Content-Type: application/json" \
  -d '{"name":"mobilenet","version":"1","type":"onnx","path":"/app/models/mobilenet.onnx"}'

# 检测模型（需指定 task + 自定义归一化参数）
curl -b cookies.txt -X POST http://localhost/models/load \
  -H "Content-Type: application/json" \
  -d '{"name":"yolov8l","version":"1","type":"onnx","path":"/app/models/yolov8l.onnx","task":"detection","labels":"/app/models/coco_80.txt","input_name":"images","output_name":"output0","input_width":640,"input_height":640,"input_mean":[0,0,0],"input_std":[1,1,1],"confidence_threshold":0.3}'
```

加载后自动持久化到 `config.json` → `dynamic_engines`，重启后自动恢复。

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

> 自定义推理引擎需实现 `InferenceEngine` 接口并注册到 `ModelFactory`，详见 [CLAUDE.md](CLAUDE.md)。

---

## 配置

### config.json

```json
{
  "server": { "port": 80, "threads": 8, "log_level": "INFO", "shutdown_timeout_ms": 30000,
              "rate_limit_req_per_sec": 1000, "rate_limit_burst": 2000 },
  "logging": { "level": "INFO", "file": "server.log" },
  "mysql": { "host": "tcp://mysql:3306", "user": "", "password": "", "database": "inference_platform", "pool_size": 10 },
  "redis": { "host": "redis", "port": 6379, "pool_size": 5 },
  "models": { "labels_path": "models/imagenet_classes.txt", "engines": {} },
  "batching": { "enabled": true, "max_batch_size": 16, "max_delay_ms": 20,
                "max_queue_delay_us": 50000, "preferred_batch_sizes": [4, 8, 16] },
  "dynamic_engines": [],
  "recommendations": { ... }
}
```

> 以上为 `stable` 配置示例。服务器启动时 ConfigAdvisor 会根据硬件自动生成 `stable`（保守）和 `aggressive`（极限）两套 profile，`server.threads`、`batching.*`、`rate_limit_*` 在首次启动时自动填入推荐值，运行中可通过 `/system/config/apply` 切换。

### 环境变量

部署时通过 Compose 环境变量覆盖 MySQL/Redis 连接信息：

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `MYSQL_USER` | `root` | 应用连接 MySQL 的用户 |
| `MYSQL_PASSWORD` | `root` | 应用连接 MySQL 的密码 |
| `REDIS_HOST` | `redis` | Redis 主机名 |
| `REDIS_PORT` | `6379` | Redis 端口 |

> Redis `host: ""`（空字符串）时，服务自动降级为**内存 Session**（无需 Redis 容器），方便本地裸跑调试。`docker-entrypoint.sh` 将环境变量注入 `/tmp/config.json`（运行时副本），不会污染持久化配置 `/app/config.json`。

### 命令行参数

| 参数 | 说明 | 示例 |
|------|------|------|
| `-c <path>` | 运行时配置文件 | `-c config.json` |
| `-P <path>` | 持久化配置文件 | `-P /app/config.json` |
| `-p <port>` | 覆盖端口 | `-p 8080` |
| `-t <n>` | I/O 线程数 | `-t 8` |
| `-l <level>` | 日志级别 | `-l DEBUG` |

CLI 参数优先级高于配置文件。`-P` 指定持久化写入目标（apply profile / saveConfig 写回此文件），默认等于 `-c`。

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
curl http://localhost/system/hardware  # 查看硬件配置
```

---

## 性能

硬件：NVIDIA RTX 5060 (8 GB), 16 核 CPU, Ubuntu 22.04

| 模型 | 延迟 (avg) | P99 | QPS |
|------|-----------|-----|-----|
| TensorRT INT8 | 13.8 ms | 15.2 ms | 142 |
| TensorRT FP16 | 16.3 ms | 20.0 ms | 152 |
| ONNX CPU | 76.3 ms | 83.6 ms | 44 |

> 使用 `bench_adaptive.py` 进行稳定版/性能版自动压测和对比。

---

## 开发

### 快速开始

```bash
# 一键构建 + 启动（推荐）
./build.sh cpu      # CPU 模式
./build.sh gpu      # GPU 模式

# 完整部署（含健康检查、模型验证）
./start.sh cpu      # CPU 部署
./start.sh gpu      # GPU 部署

# 仅本地编译（不启动 Docker）
./start.sh --build-only cpu

# 使用开发 Compose（源码挂载，重启即重编译）
docker compose -f docker-compose.dev.yml up -d --build
# C++ 改动后无需重建镜像：
docker compose -f docker-compose.dev.yml restart
```

`build.sh` 是轻量 wrapper（cmake + make → docker compose up），`start.sh` 包含完整流程（检查前置条件 → 验证模型 → 编译 → 启动 Docker → 健康检查）。开发容器启动时自动 `cmake .. && make -j$(nproc)`，之后只需 `restart` 就能生效改动。源码全量挂载到容器内 `/project`。

### ASAN 调试

```bash
# CMakeLists.txt 中启用 ASAN（默认关，因为与 CUDA 不兼容）
cmake .. -DENABLE_TENSORRT=OFF -DENABLE_ASAN=ON && make -j$(nproc)
```

### 文档索引

| 文档 | 内容 |
|------|------|
| [CLAUDE.md](CLAUDE.md) | 构建命令、架构细节、AI 助手使用指南 |
| [AGENTS.md](AGENTS.md) | AI 助手规则 |

---

## 已知限制

- **Linux only**：muduo 基于 epoll，不支持 Windows/macOS
- **模型文件不在仓库**：~470 MB，需单独放置
- **GPU 推理串行**：`gpu_mutex_` 同一时刻一个 GPU 任务
- **ASAN 与 CUDA 不兼容**：ASAN 影子内存与 CUDA 驱动冲突，GPU 模式下需关闭
- **stb_image 全局状态**：JPEG decode 已用窄锁串行化，多模型高并发下 decode 吞吐受锁竞争影响

---

## 致谢

[muduo](https://github.com/chenshuo/muduo) · [ONNX Runtime](https://github.com/microsoft/onnxruntime) · [TensorRT](https://developer.nvidia.com/tensorrt) · [spdlog](https://github.com/gabime/spdlog) · [nlohmann/json](https://github.com/nlohmann/json) · [stb](https://github.com/nothings/stb)
