# Benchmark 测试手册

## 一、前置要求

```bash
# 1. 编译工具
sudo apt-get install build-essential cmake

# 2. Google Benchmark
sudo apt-get install libbenchmark-dev

# 3. Boost (用于 Asio 对照组)
sudo apt-get install libboost-all-dev

# 4. liburing (io_uring 支持)
sudo apt-get install liburing-dev

# 5. 内核版本 ≥ 5.6（io_uring 支持）
uname -r
```

## 二、构建

```bash
cd MyIOMessageEngine
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## 三、一键全量测试

```bash
# 执行所有 benchmark（MPSC + 协程 + IO 对比）
bash benchmark/scripts/run_all_bench.sh

# 只跑 IO 对比
bash benchmark/scripts/run_all_bench.sh --io-only
```

## 四、单独测试

### 4.1 MPSC 无锁队列

```bash
bash benchmark/scripts/run_mpsc_bench.sh
```

**测试内容**：4 个生产者线程并发向 1 个消费者注入任务  
**对比对象**：RingBufferMPSC vs std::mutex+queue vs Boost.Lockfree  
**关注指标**：Wall Time (ns/op)

### 4.2 协程开销

```bash
bash benchmark/scripts/run_coroutine_bench.sh
```

**测试内容**：  
- 协程帧创建+销毁（测内存池效率）
- 完整生命周期（create → suspend → resume → return → destroy）
- 对称传输 100 层嵌套（测 Symmetric Transfer 性能）

**关注指标**：ns/op

### 4.3 IO Echo 吞吐对比

```bash
# 默认 2 次重复
bash benchmark/scripts/run_io_bench.sh

# 5 次重复（更精确，但更慢）
bash benchmark/scripts/run_io_bench.sh 5
```

**测试内容**：Echo Server 在 1/2/4/8/16/32/64/128 并发下的吞吐  
**对比对象**：io_uring (framework_test) vs Boost.Asio epoll (asio_echo_server)  
**关注指标**：bytes_per_second (MiB/s)  
**输出**：Side-by-Side 对比表格

## 五、手动测试（不用脚本）

### 5.1 手动跑 MPSC Benchmark

```bash
cd build
./mpsc_bench --benchmark_repetitions=5
```

### 5.2 手动跑 IO Echo 对比

```bash
cd build

# 终端 1：启动 io_uring server
./framework_test

# 终端 2：启动 Asio server
./asio_echo_server 8888

# 终端 3：测试 io_uring (port 8889)
PORT=8889 ./echo_client_bench --benchmark_repetitions=3

# 终端 4：测试 Asio (port 8888)
PORT=8888 ./echo_client_bench --benchmark_repetitions=3
```

### 5.3 手动测试 MPSC Gateway Demo

```bash
cd build

# 终端 1：启动 gateway
./mpsc_gateway_demo

# 终端 2：发送并发请求
for i in $(seq 1 100); do echo "request-$i" | nc -q0 localhost 9000 & done

# 观察 Monitor 输出的 Requests / Replies / MPSC→Worker pending

# Ctrl+C 停止
```

## 六、结果解读

### 6.1 MPSC Queue

| 指标 | 含义 |
|------|------|
| Wall Time | 从 push 到完成的真实耗时（包含等待） |
| CPU Time | 实际消耗的 CPU 时间 |
| **关注 Wall Time** | 它反映了用户感知的延迟 |

### 6.2 Coroutine

| 指标 | 预期值 | 超过此值说明有问题 |
|------|-------|-------------------|
| CreateAndDestroy | < 10ns | > 50ns（内存池可能没生效） |
| FullLifecycle | < 20ns | > 100ns |
| SymmetricTransfer/100 | < 3000ns | > 10000ns（可能没开 -O3） |

### 6.3 IO Echo

| 场景 | io_uring 预期表现 |
|------|------------------|
| Thread 1 | 可能略慢于 Asio（无合批优势） |
| Thread 4-32 | 应持平或略优 |
| Thread 64+ | 应展现合批优势 |

> **重要**：只关注**同一次运行内**的相对对比，不要跨次/跨天对比绝对值（VM 环境波动大）。

## 七、常见问题

### Q: benchmark 输出 "Library was built as DEBUG"
```bash
# 重新以 Release 构建
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Q: echo_client_bench 报 "Connection refused"
确认 echo server 已在对应端口启动：
```bash
ss -tlnp | grep -E '888[89]'
```

### Q: io_uring server 性能不如预期
检查 `wait_cqe_timeout` 的影响。如果在纯 Echo 场景下不需要 MPSC 跨线程投递，可以临时改回 `wait_cqe` 测试：
```cpp
// scheduler.hpp line 170
// 改为 wait_cqe 测试纯 io_uring 性能
if (io_context_.wait_cqe(&cqe) == 0) {  // 替代 wait_cqe_timeout
```

### Q: 想看更详细的数据
```bash
# JSON 格式输出
./mpsc_bench --benchmark_format=json --benchmark_out=result.json

# CSV 格式
./mpsc_bench --benchmark_format=csv --benchmark_out=result.csv
```

## 八、文件结构

```
benchmark/
├── scripts/
│   ├── run_all_bench.sh        # 一键全量测试
│   ├── run_mpsc_bench.sh       # MPSC 队列测试
│   ├── run_coroutine_bench.sh  # 协程开销测试
│   └── run_io_bench.sh         # IO Echo 对比测试
├── mpsc_bench.cc               # MPSC 源码
├── coroutine_bench.cc          # 协程 benchmark 源码
├── echo_client_bench.cc        # Echo 客户端 benchmark
├── asio_echo_server.cc         # Boost.Asio Echo Server (对照组)
└── BENCHMARK_PROGRESS.md       # 进度记录

bench_docs/
├── mpsc_bench_result.md        # MPSC 分析文档
├── coroutine_bench_result.md   # 协程分析文档
├── io_bench_result.md          # IO 对比分析文档
└── *_YYYYMMDD_HHMMSS.txt       # 每次运行的原始结果
```
