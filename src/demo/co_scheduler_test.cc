#include "../include/async_logger.hpp"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

// 假设这些是我们已经写好的头文件
#include "../include/scheduler.hpp"
#include "../include/task.hpp"

constexpr int NUM_PRODUCERS = 4; // 模拟 4 个并发生产者
constexpr int TASKS_PER_PRODUCER =
    250000; // 每个生产者投递 25 万个任务 (总 100 万)
constexpr int TOTAL_TASKS =
    NUM_PRODUCERS * TASKS_PER_PRODUCER; // 总计 100 万任务

// 全局原子计数器，用于主线程校验所有任务是否执行完毕
std::atomic<int> completed_tasks{0};

// 我们的核心协程函数
Task<int> simple_coroutine(int id) {
  // 模拟一些基于局部变量的极速计算
  int result = id * 2;

  // 任务走到生命周期终点，原子计数器 +1
  // 使用 memory_order_release 保证：协程内的所有状态修改，
  // 在这行计数器 +1 的操作之后，对主线程绝对可见。
  completed_tasks.fetch_add(1, std::memory_order_release);

  co_return result;
}

void producer_thread(CoroutineScheduler &orchestrator, int producer_id) {
  for (int i = 0; i < TASKS_PER_PRODUCER; ++i) {
    // 生成一个全局唯一的任务 ID
    int task_id = producer_id * TASKS_PER_PRODUCER + i;

    // 极速创建协程，并无锁 Push 进调度引擎
    // 此时协程内部处于 initial_suspend (惰性挂起) 状态
    orchestrator.submit(simple_coroutine(task_id));
  }
}

// 将指定线程绑定到某个 CPU 核心
static void pin_to_cpu(std::thread &t, int cpu_id) {
#ifdef __linux__
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu_id, &cpuset);
  pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
#endif
}

int main() {
  ASYNC_LOG() << " 正在启动无锁协程调度引擎...\n";

  // 初始化调度器，RingBuffer 容量设为 1048576 (满足 100 万任务无阻推入)
  // Worker 线程钉死在 CPU 0 上，防止 OS/Hypervisor 跨核迁移抖动
  CoroutineScheduler orchestrator(1048576, /*worker_cpu_id=*/0);
  orchestrator.start();

  auto start_time = std::chrono::high_resolution_clock::now();

  // 启动多核生产者，开始疯狂轰炸
  // 将每个生产者分别绑定到 CPU 1, 2, 3, 0（循环分配）
  std::vector<std::thread> producers;
  for (int i = 0; i < NUM_PRODUCERS; ++i) {
    producers.emplace_back(
        std::thread(producer_thread, std::ref(orchestrator), i));
    // 生产者绑定到 CPU 1, 2, 3（避开 Worker 所在的 CPU 0）
    pin_to_cpu(producers.back(), (i % 3) + 1);
  }

  // 生产者线程回收
  for (auto &p : producers) {
    p.join();
  }

  // 主线程也绑核，避免与 Worker（CPU 0）抢占
  {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(3, &cpuset); // 主线程绑到 CPU 3，此时生产者已经 join 完毕不再竞争
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
#endif
  }

  // 主线程自旋等待所有协程在消费者线程中执行完毕
  // 使用轻量级 pause 忙等，不调用 yield 避免陷入内核态
  while (completed_tasks.load(std::memory_order_acquire) < TOTAL_TASKS) {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#endif
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> elapsed = end_time - start_time;

  ASYNC_LOG() << "[message] 引擎运行完毕！\n";
  ASYNC_LOG() << "总计处理任务: " << TOTAL_TASKS << "\n";
  ASYNC_LOG() << "总耗时: " << elapsed.count() << " ms\n";
  ASYNC_LOG() << "吞吐量 (OPS): " << (TOTAL_TASKS / (elapsed.count() / 1000.0))
              << " 任务/秒\n";

  // 优雅停机
  orchestrator.stop();
  return 0;
}