#pragma once

#include "scheduler.hpp"
#include "task.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace Engine {

/**
 * @brief 运行时环境 (Runtime Environment)
 *
 * 这是一个单例/静态类，用于在当前 Worker 线程中隐式持有调度器和 IO 上下文。
 * 这样业务代码在创建 TcpStream 读写或 spawn 新任务时，就不需要再手动传递
 * Scheduler 或 IoContext 的引用了。
 */
class Runtime {
private:
  // 使用 thread_local 保证每个 Worker 线程（哪怕以后扩展为多核多 Worker）
  // 都有自己独立的调度器指针，避免锁竞争。
  inline static thread_local CoroutineScheduler *current_scheduler_ = nullptr;

public:
  // 阶段一在 net.hpp 中占位使用的 g_ctx，现在我们把它统一收口到这里。
  // 注意：需要将 net.hpp 中的 struct Runtime 定义删除，统一使用这个。

  /**
   * @brief 供底层组件（如 io_awaiter, TcpStream）获取当前线程的 IO 上下文
   */
  static IoContext &get_io_context() {
    if (!current_scheduler_) {
      throw std::runtime_error(
          "Runtime is not initialized in this thread. Cannot get IoContext.");
    }
    return current_scheduler_->get_io_context();
  }

  /**
   * @brief 供底层或系统级代码获取当前的调度器
   */
  static CoroutineScheduler &get_scheduler() {
    if (!current_scheduler_) {
      throw std::runtime_error(
          "Runtime is not initialized in this thread. Cannot get Scheduler.");
    }
    return *current_scheduler_;
  }

  /**
   * @brief 极简的任务投递入口 (Public API)
   * 业务代码只需调用 Runtime::spawn(my_coroutine());
   * 即可将新任务放入 MPSC 队列中调度。
   */
  static bool spawn(Task<int> task) {
    if (!current_scheduler_) {
      throw std::runtime_error(
          "Runtime is not initialized in this thread. Cannot spawn task.");
    }
    return current_scheduler_->submit(std::move(task));
  }

  // --- 框架初始化预留接口 (供阶段三 block_on 使用) ---

  // 由框架的 block_on 入口在启动 Worker 前调用，绑定当前线程的调度器
  static void set_current_scheduler(CoroutineScheduler *sched) {
    current_scheduler_ = sched;
  }
};

// 全局退出标志位
inline std::atomic<bool> g_stop_flag{false};

inline void signal_handler(int signum) {
  if (signum == SIGINT || signum == SIGTERM) {
    g_stop_flag.store(true, std::memory_order_release);
  }
}

/**
 * @brief 运行时构建器，用于配置并启动整个框架
 */
class RuntimeBuilder {
private:
  size_t queue_capacity_ = 65536;
  int worker_cpu_id_ = -1;
  unsigned int io_uring_entries_ = 4096;

public:
  RuntimeBuilder &set_queue_capacity(size_t capacity) {
    queue_capacity_ = capacity;
    return *this;
  }

  RuntimeBuilder &set_bind_cpu(int cpu_id) {
    worker_cpu_id_ = cpu_id;
    return *this;
  }

  RuntimeBuilder &set_io_uring_entries(unsigned int entries) {
    io_uring_entries_ = entries;
    return *this;
  }

  /**
   * @brief 接管生命周期，阻塞执行主任务
   */
  int block_on(Task<int> main_task) {
    // 1. 注册信号处理 (Graceful Shutdown)
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 2. 初始化底层的 MPSC 调度器和 io_uring 实例
    CoroutineScheduler sched(queue_capacity_, worker_cpu_id_,
                             io_uring_entries_);

    // 3. 为主线程绑定环境，以便主协程或者后续逻辑可以使用 Runtime::spawn
    Runtime::set_current_scheduler(&sched);

    // 4. 将业务入口任务（如 server_main）投递到引擎
    sched.submit(std::move(main_task));

    // 5. 启动引擎，并在 Worker 线程跑起来的瞬间，将它的环境绑定好
    sched.start([&sched]() { Runtime::set_current_scheduler(&sched); });

    // 6. 接管主线程控制权，死循环监控退出信号
    // 使用 100ms 一次的轮询阻塞，将 CPU 消耗降至最低，把所有的算力让给 Worker
    while (!g_stop_flag.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\n[Runtime] Received termination signal (Ctrl+C). Initiating "
                 "graceful shutdown..."
              << std::endl;

    // 7. 通知底层调度器安全停止，等待队列消费完并释放所有资源
    sched.stop();
    std::cout << "[Runtime] Shutdown complete." << std::endl;
    return 0;
  }
};

} // namespace Engine
