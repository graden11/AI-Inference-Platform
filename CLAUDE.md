# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Environment: Git Bash + WSL dual-context

The project lives on Windows (`D:\jetbrains\clion-project\httpserver`) and is accessed from Git Bash at `/d/jetbrains/clion-project/httpserver/`. The Docker daemon and all containers run inside WSL, accessed via the `wsl` command (e.g. `wsl docker ps`).

**CRITICAL — file existence checks**: The Git Bash filesystem (`/d/...`) and WSL filesystem (`/mnt/d/...`) are NOT always in sync — WSL may not have the D: drive mounted. Before concluding a file or directory does not exist, check BOTH contexts:
1. Directly from Git Bash: `ls /d/jetbrains/clion-project/httpserver/path/to/file`
2. From WSL: `wsl ls /mnt/d/jetbrains/clion-project/httpserver/path/to/file`
3. Inside running Docker containers: `wsl docker exec <container> ls /path`

If any one of these finds the file, it exists. Never conclude "missing" after checking only one context. If a service is running and responding normally, treat "file not found" as a likely error in the check itself — re-verify before suggesting downloads or rebuilds.

## Build & Run

```bash
# One-shot: cmake + make + docker compose up (gpu or cpu)
./build.sh cpu      # cloud deployment
./build.sh gpu      # local with NVIDIA GPU

# Manual build (inside dev container or bare metal)
mkdir -p build && cd build
cmake .. -DENABLE_TENSORRT=OFF   # CPU-only
cmake .. -DENABLE_TENSORRT=ON    # with TensorRT GPU inference
make -j$(nproc)

# Run directly (not in Docker)
cd build && ./simple_server -c ../WebApps/InferenceServer/config.json -p 80 -t 4

# Docker management
docker compose up -d                          # GPU
docker compose -f docker-compose.cpu.yml up -d # CPU
docker compose logs -f httpserver
docker compose down && docker compose up -d --build
```

The project builds to a **single binary** `simple_server` that contains both the HTTP framework and the InferenceServer application. There are no separate libraries or plugins.

## Architecture

### Two-layer design

1. **HttpServer/** — Reusable C++ HTTP framework built on muduo. Provides Router, MiddlewareChain, SessionManager, CorsMiddleware, MetricsMiddleware, SSL support, DB connection pool, and config loading. Has no dependency on InferenceServer.

2. **WebApps/InferenceServer/** — Application layer. Registers 14 route handlers, manages online users, session tracking, initializes the ModelFactory for inference, and orchestrates dynamic batching.

### Request flow

```
TCP → muduo TcpServer → HttpContext (parse) → MiddlewareChain::before()
→ Router::route() → Handler::handle() → MiddlewareChain::after() → TCP response
```

Middleware runs in order: MetricsMiddleware (records latency), then CorsMiddleware (handles OPTIONS preflight).

### Handler registration

All 14 handlers are registered in `InferenceServer::initializeRouter()` (`WebApps/InferenceServer/src/InferenceServer.cpp`). Handlers use the **friend class** pattern — each handler class is declared `friend` in `InferenceServer.h` so it can access private state (`onlineUsers_`, `loginSessions_`, `mysqlUtil_`, `modelFactory_`).

Routes:

| Method | Path | Handler | Auth |
|--------|------|---------|------|
| GET | `/`, `/entry` | EntryHandler | No |
| POST | `/login` | LoginHandler | No |
| POST | `/register` | RegisterHandler | No |
| POST | `/user/logout` | LogoutHandler | Yes |
| GET | `/menu` | MenuHandler | Yes |
| GET | `/backend` | GameBackendHandler | Yes |
| GET | `/backend_data` | (lambda) | No |
| POST | `/predict` | PredictHandler | No |
| POST | `/predict/proto` | ProtoPredictHandler | No |
| GET | `/metrics` | MetricsHandler | No |
| POST | `/models/load` | ModelLoadHandler | Yes |
| GET | `/models` | ModelListHandler | No |
| DELETE | `/models/:name/:version` | ModelUnloadHandler | Yes |

### Routing

Two registration styles coexist:
- `httpServer_.Get("/path", handlerPtr)` — exact match
- `httpServer_.addRoute(HttpRequest::kGet, "/path/(\\d+)", callback)` — regex match with capture groups

### Session & auth

- Sessions stored in Redis (`RedisSessionStorage`) with JSON serialization and TTL-based expiry
- Falls back to in-memory (`MemorySessionStorage`) when no Redis config is provided
- Login sets `Set-Cookie: SESSIONID=<uuid>`, subsequent requests carry it
- Handlers check `session->getValue("isLoggedIn") == "true"`
- `loginSessions_` map (userId → sessionId) enables forced logout from the backend panel
- Session data: `userId`, `username`, `isLoggedIn` (3 keys stored as JSON in Redis)

### Inference engines

`ModelFactory` (`WebApps/InferenceServer/src/ModelFactory.cpp`) supports **versioned model storage**: `name → (version → shared_ptr<InferenceEngine>)`. Models are addressed as `"name:version"` (e.g. `"resnet50:v2"`), omitting version returns the latest.

- `"onnx"` type → `ResNet50Engine` (CPU, ONNX Runtime)
- `"tensorrt"` type → `ResNet50TRTEngine` (GPU, TensorRT)

GPU inference is serialized with `gpu_mutex_` — one request at a time. Thread safety uses `shared_mutex` (shared_lock for reads, unique_lock for writes). `shared_ptr` ownership ensures in-flight inferences survive model unload.

### Dynamic model management

Models can be loaded/unloaded at runtime without restart:

- `POST /models/load` — requires login. Loads a new model version, persists to `config.json` → `dynamic_engines`
- `GET /models` — lists all loaded models with name, version, type, path, is_latest
- `DELETE /models/:name/:version` — requires login. Unloads a version, recomputes latest, persists config

Static models (in `config.json` → `models.engines`) are loaded at startup. Dynamic models (in `dynamic_engines`) persist across restarts.

### Dynamic batching

`RequestBatcher` collects incoming prediction requests into batches to improve GPU/CPU throughput:

- Config: `batching.enabled` (default false), `max_batch_size`, `max_delay_ms`
- Worker thread waits for first request, collects up to `max_batch_size` or `max_delay_ms`, groups by model name
- Each group dispatched to `engine->predictBatch()` for single-inference-call processing
- Falls back to sequential `predictFromBytes()` when batching disabled or engine lacks batch support
- ONNX: builds `{N, C, H, W}` tensor, single `session_->Run()`
- TensorRT: pre-allocates `maxBatchSize` pinned + device buffers, sets dynamic input shape, single GPU enqueue

### Graceful shutdown

SIGINT/SIGTERM → `sigwait()` thread → `HttpServer::gracefulShutdown()`:
1. Stops accepting new connections (`accepting_=false`)
2. Drains in-flight requests (polls `inflightCount_` via atomic counter)
3. Timeout (config `shutdown_timeout_ms`, default 30s) or count reaches 0 → `mainLoop_.quit()`

### Config flow

`main.cpp` parses CLI args in **two passes**: first to find `-c <configPath>`, then to override port/threads/log_level from CLI. So `-p 8080` always wins over what's in config.json.

Config structure (`config.json`):
- `server` — port, threads, log_level, shutdown_timeout_ms
- `logging` — spdlog level + file
- `mysql` — host, user, password, database, pool_size
- `redis` — host, port, pool_size (empty host = in-memory sessions)
- `models` — `labels_path` + `engines` (static models: name → {type, version?, path})
- `batching` — `enabled`, `max_batch_size`, `max_delay_ms`
- `dynamic_engines` — persisted by `/models/load` API, restored on restart

### HTML resources

Handlers read HTML files at runtime via relative path `../WebApps/InferenceServer/resource/<name>.html` from CWD. In Docker, the resource files are copied to `/WebApps/InferenceServer/resource/` and CWD is `/app`, so the relative path resolves correctly. This path dependency means the binary must be run from the right directory.

## Key constraints

- **Linux only**: muduo uses epoll, no Windows/macOS support
- **Models not in git**: `WebApps/InferenceServer/models/` must be populated before first run (~174 MB)
- **Passwords are bcrypt-hashed** using Linux crypt_r() with $2b$ format
- **Single binary**: all routes compiled in, no hot-reload or plugin system
- **When `ENABLE_TENSORRT=OFF`**, `ResNet50TRTEngine.cpp` is excluded from the build via `list(FILTER ... EXCLUDE REGEX)`
- **Dockerfile needs pre-built binary**: the production Dockerfile copies `build/simple_server`, so you must run `cmake` + `make` before `docker compose build`
