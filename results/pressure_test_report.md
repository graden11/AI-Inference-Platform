# InferenceServer 动态批处理压测报告

## 1. 测试结论

本轮压测验证了新的动态批处理参数对吞吐和延迟的影响。测试对象为 `squeezenet1.1-7_trt:1`，接口为 `/predict/raw`，客户端使用长连接复用，服务端运行在性能模式下。

综合结果：

- 当前性能模式配置 A 已经具备较好的吞吐与低并发延迟平衡。
- 配置 B 峰值吞吐略高于 A，但低并发延迟明显变差。
- 配置 C 峰值吞吐与 B 基本持平，但低并发延迟代价最大，不建议采用。
- 推荐保留配置 A 作为默认性能模式；如只追求压测峰值，可临时使用配置 B。

峰值结果对比：

| 配置 | 参数 | 最佳并发 | 峰值 QPS | P95 延迟 | 平均 Batch | Timeout | Preferred |
|---|---|---:|---:|---:|---:|---:|---:|
| A | preferred=[4,8,16,64], delay=20ms | 128 | 1025.7 | 146.7ms | 15.8 | 0% | 100% |
| B | preferred=[8,16,32,64], delay=50ms | 128 | 1042.6 | 144.8ms | 16.7 | 0% | 100% |
| C | preferred=[16,32,64], delay=80ms | 128 | 1043.6 | 144.1ms | 17.4 | 0% | 100% |

配置 C 相比 B 仅提升约 1 QPS，收益可以忽略，但在并发 1-8 时延迟显著恶化。因此 C 不适合作为默认配置。

## 2. 测试环境

项目路径：

`D:\jetbrains\clion-project\httpserver`

运行环境：

- Windows + WSL2 + Docker
- 服务容器：`kama-httpserver`
- GPU：NVIDIA GeForce RTX 5060
- 服务端线程数：12
- TensorRT FP16：开启
- 最大 Batch Size：64
- 测试模型：`squeezenet1.1-7_trt:1`

测试脚本：

`scripts/bench_fixed.py`

脚本特性：

- 每个 worker 复用一个 `HTTPConnection`
- 使用 HTTP/1.1 Keep-Alive
- 不自动切配置、不自动重启服务、不自动加载模型
- 每个并发档位先 warmup，再正式计量
- 同时记录客户端指标和服务端 `/metrics/json` 增量指标

测试参数：

- 并发阶梯：`1, 2, 4, 8, 16, 32, 64, 128`
- 每档 warmup：5 秒
- 每档正式压测：30 秒
- 错误率：三组测试均为 0%

## 3. 配置说明

### 配置 A：当前性能模式

```json
{
  "max_batch_size": 64,
  "max_queue_delay_us": 20000,
  "preferred_batch_sizes": [4, 8, 16, 64]
}
```

该配置低并发触发门槛低，并发达到 4 时即可 preferred dispatch，适合同时兼顾交互延迟和吞吐。

### 配置 B：均衡吞吐模式

```json
{
  "max_batch_size": 64,
  "max_queue_delay_us": 50000,
  "preferred_batch_sizes": [8, 16, 32, 64]
}
```

该配置从 batch=8 开始 preferred dispatch，吞吐略高，但并发低于 8 时需要等待 timeout。

### 配置 C：高吞吐模式

```json
{
  "max_batch_size": 64,
  "max_queue_delay_us": 80000,
  "preferred_batch_sizes": [16, 32, 64]
}
```

该配置从 batch=16 才开始 preferred dispatch，低并发请求等待时间较长，适合离线批处理，不适合交互式请求。

## 4. 详细结果

### 配置 A

| 并发 | QPS | P50 | P95 | P99 | Avg Batch | Timeout | Preferred | Preprocess | GPU | ExecQ |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 12.0 | 73.5ms | 122.7ms | 132.9ms | 1.0 | 100% | 0% | 58.1ms | 1.6ms | 0.1ms |
| 2 | 78.7 | 24.9ms | 28.5ms | 34.2ms | 2.0 | 100% | 0% | 0.1ms | 1.9ms | 0.1ms |
| 4 | 411.0 | 10.8ms | 14.9ms | 17.0ms | 4.0 | 0% | 100% | 0.1ms | 1.7ms | 0.1ms |
| 8 | 631.7 | 12.0ms | 21.7ms | 26.4ms | 4.0 | 0% | 100% | 0.2ms | 2.6ms | 0.5ms |
| 16 | 770.7 | 19.7ms | 32.2ms | 38.3ms | 4.1 | 0% | 100% | 0.2ms | 3.2ms | 6.3ms |
| 32 | 799.1 | 39.3ms | 53.7ms | 60.6ms | 4.3 | 0% | 100% | 0.3ms | 3.3ms | 23.7ms |
| 64 | 905.0 | 69.9ms | 88.1ms | 96.6ms | 8.1 | 0% | 100% | 0.9ms | 4.4ms | 44.2ms |
| 128 | 1025.7 | 124.1ms | 146.7ms | 155.3ms | 15.8 | 0% | 100% | 2.6ms | 5.9ms | 76.2ms |

### 配置 B

| 并发 | QPS | P50 | P95 | P99 | Avg Batch | Timeout | Preferred | Preprocess | GPU | ExecQ |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 8.7 | 104.7ms | 156.2ms | 163.3ms | 1.0 | 100% | 0% | 59.8ms | 1.5ms | 0.1ms |
| 2 | 36.2 | 54.7ms | 60.0ms | 64.4ms | 2.0 | 100% | 0% | 0.1ms | 1.8ms | 0.1ms |
| 4 | 70.1 | 56.4ms | 61.3ms | 64.4ms | 4.0 | 100% | 0% | 0.2ms | 2.1ms | 0.1ms |
| 8 | 536.4 | 15.3ms | 19.7ms | 21.7ms | 8.0 | 0% | 100% | 0.2ms | 2.2ms | 0.1ms |
| 16 | 792.3 | 19.7ms | 30.3ms | 35.6ms | 8.0 | 0% | 100% | 0.4ms | 3.7ms | 0.8ms |
| 32 | 917.3 | 34.1ms | 48.8ms | 56.0ms | 8.1 | 0% | 100% | 0.6ms | 4.5ms | 12.4ms |
| 64 | 930.3 | 68.0ms | 86.2ms | 95.2ms | 8.6 | 0% | 100% | 0.8ms | 4.7ms | 43.5ms |
| 128 | 1042.6 | 121.7ms | 144.8ms | 155.2ms | 16.7 | 0% | 100% | 2.6ms | 6.0ms | 79.1ms |

### 配置 C

| 并发 | QPS | P50 | P95 | P99 | Avg Batch | Timeout | Preferred | Preprocess | GPU | ExecQ |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 6.8 | 134.3ms | 195.2ms | 277.5ms | 1.0 | 100% | 0% | 62.1ms | 1.5ms | 0.1ms |
| 2 | 23.3 | 84.6ms | 91.2ms | 97.8ms | 2.0 | 100% | 0% | 0.1ms | 1.8ms | 0.1ms |
| 4 | 45.8 | 86.5ms | 91.7ms | 93.6ms | 4.0 | 100% | 0% | 0.2ms | 2.1ms | 0.1ms |
| 8 | 89.0 | 88.5ms | 96.5ms | 100.2ms | 8.0 | 100% | 0% | 0.3ms | 2.5ms | 0.1ms |
| 16 | 699.2 | 23.0ms | 27.2ms | 29.2ms | 16.0 | 0% | 100% | 0.4ms | 3.1ms | 0.1ms |
| 32 | 994.6 | 31.8ms | 41.1ms | 48.5ms | 16.0 | 0% | 100% | 1.1ms | 5.7ms | 1.0ms |
| 64 | 1038.4 | 61.3ms | 76.3ms | 83.7ms | 16.0 | 0% | 100% | 1.9ms | 6.2ms | 26.9ms |
| 128 | 1043.6 | 121.4ms | 144.1ms | 153.8ms | 17.4 | 0% | 100% | 2.2ms | 6.4ms | 80.3ms |

## 5. 分析

### 5.1 动态批处理已经生效

三组配置在达到 preferred batch size 后，均表现为：

- Timeout dispatch 降为 0%
- Preferred dispatch 达到 100%
- 平均 batch size 随 preferred batch size 提高而提高
- 吞吐从低并发逐步爬升到 1000+ QPS

说明当前动态批处理调度机制工作正常，已经能按 preferred batch size 主动发车。

### 5.2 低并发延迟由 preferred 门槛决定

配置 A 的最小 preferred batch size 是 4，因此并发达到 4 后即可立即触发 preferred dispatch。

配置 B 的最小 preferred batch size 是 8，因此并发 4 仍然只能等待 timeout，P95 从 A 的 14.9ms 上升到 61.3ms。

配置 C 的最小 preferred batch size 是 16，因此并发 8 仍然只能等待 timeout，P95 达到 96.5ms。

这说明 preferred batch size 不能只按峰值吞吐设置，还必须考虑典型在线请求并发。

### 5.3 峰值吞吐差距很小

在并发 128 下：

- A：1025.7 QPS
- B：1042.6 QPS
- C：1043.6 QPS

B 相比 A 提升约 1.6%，C 相比 B 提升约 0.1%。C 的吞吐收益几乎不可见，但低并发延迟损失非常明显。

### 5.4 Executor queue wait 是高并发下的主要排队项

在并发 128 下：

- A ExecQ：76.2ms
- B ExecQ：79.1ms
- C ExecQ：80.3ms

这说明系统在高并发下主要延迟来自 executor 排队，而不是 GPU 单次推理本身。GPU 推理耗时约 5.9-6.4ms，已经比较稳定。

## 6. 推荐配置

### 默认推荐：配置 A

```json
{
  "max_batch_size": 64,
  "max_queue_delay_us": 20000,
  "preferred_batch_sizes": [4, 8, 16, 64]
}
```

推荐理由：

- 低并发延迟最好
- 并发 4 即可进入 preferred dispatch
- 峰值 QPS 已超过 1000
- 相比 B/C，吞吐只低约 1.6%，但交互体验明显更稳

### 压测冲峰值可选：配置 B

```json
{
  "max_batch_size": 64,
  "max_queue_delay_us": 50000,
  "preferred_batch_sizes": [8, 16, 32, 64]
}
```

适用场景：

- 压测环境
- 离线批量任务
- 典型并发稳定高于 8

不建议作为默认在线配置。

### 不推荐：配置 C

```json
{
  "max_batch_size": 64,
  "max_queue_delay_us": 80000,
  "preferred_batch_sizes": [16, 32, 64]
}
```

不推荐理由：

- 峰值吞吐相比 B 几乎没有提升
- 并发 1-8 全部 timeout dispatch
- 低并发延迟明显恶化
- 更适合纯离线批处理，不适合当前项目的前端交互式推理

## 7. 最终评价

本轮压测说明，当前项目的动态批处理机制已经从“是否可用”进入“参数调优”阶段。服务在 TensorRT + 动态批处理下，单模型峰值达到约 1000 QPS，且错误率为 0%，已经具备非常漂亮的简历展示价值。

推荐最终对外表述：

> 基于 muduo + TensorRT 实现异步推理服务与动态批处理调度，支持 preferred batch size 与 max queue delay 配置。通过 Keep-Alive 压测脚本和服务端 metrics 对调度策略进行验证，squeezenet TensorRT 单模型峰值吞吐达到约 1000 QPS，较优化前约 28 QPS 提升约 36 倍。

