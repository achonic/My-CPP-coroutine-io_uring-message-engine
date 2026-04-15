#include "mpsc_queue.hpp"
#include <atomic>
#include <benchmark/benchmark.h>
#include <boost/lockfree/queue.hpp>
#include <mutex>
#include <queue>
#include <thread>

/**
 * @brief 测试对照组：标准互斥锁队列
 */
template <typename T> class MutexQueue {
  std::queue<T> q_;
  std::mutex mtx_;

public:
  void push(T val) {
    std::lock_guard<std::mutex> lock(mtx_);
    q_.push(std::move(val));
  }
  bool pop(T &val) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (q_.empty())
      return false;
    val = std::move(q_.front());
    q_.pop();
    return true;
  }
};

struct DummyTask {
  int id;
  char padding[64];
};

/**
 * MPSC 性能测试：多生产者，单消费者
 * 模拟真实的调度器注入场景
 */
static void BM_MPSC_Throughput(benchmark::State &state) {
  static RingBufferMPSC<DummyTask> queue(1048576);
  static std::atomic<bool> producer_running{true};
  static std::thread consumer;

  // 在0号线程启动一个消费者线程
  if (state.thread_index() == 0) {
    producer_running = true;
    consumer = std::thread([]() {
      DummyTask task;
      while (producer_running) {
        queue.pop(task);
      }
    });
  }

  DummyTask task{1, ""};
  for (auto _ : state) {
    // 使用 try_push 以防队列爆掉影响测试结果
    while (!queue.try_push(task)) {
      __builtin_ia32_pause();
    }
  }

  // 清理
  if (state.thread_index() == 0) {
    producer_running = false;
    if (consumer.joinable())
      consumer.join();
  }
}
// 使用 4 个线程并发生产，模拟 4 核注入单核
BENCHMARK(BM_MPSC_Throughput)->Threads(4);

/**
 * 对照组：多线程 Mutex 竞争
 */
static void BM_Mutex_Throughput(benchmark::State &state) {
  static MutexQueue<DummyTask> queue;
  static std::atomic<bool> producer_running{true};
  static std::thread consumer;

  if (state.thread_index() == 0) {
    producer_running = true;
    consumer = std::thread([]() {
      DummyTask task;
      while (producer_running) {
        queue.pop(task);
      }
    });
  }

  DummyTask task{1, ""};
  for (auto _ : state) {
    queue.push(task);
  }

  if (state.thread_index() == 0) {
    producer_running = false;
    if (consumer.joinable())
      consumer.join();
  }
}
BENCHMARK(BM_Mutex_Throughput)->Threads(4);

/**
 * 对照组：Boost Lockfree Queue (MPMC)
 */
static void BM_BoostLockFree_Throughput(benchmark::State &state) {
  static boost::lockfree::queue<DummyTask> queue(1048576);

  static std::atomic<bool> producer_running{true};
  static std::thread consumer;

  if (state.thread_index() == 0) {
    producer_running = true;
    consumer = std::thread([]() {
      DummyTask task;
      while (producer_running) {
        queue.pop(task);
      }
    });
  }

  DummyTask task{1, ""};
  for (auto _ : state) {
    while (!queue.push(task)) {
      __builtin_ia32_pause();
    }
  }

  if (state.thread_index() == 0) {
    producer_running = false;
    if (consumer.joinable())
      consumer.join();
  }
}
BENCHMARK(BM_BoostLockFree_Throughput)->Threads(4);

BENCHMARK_MAIN();
