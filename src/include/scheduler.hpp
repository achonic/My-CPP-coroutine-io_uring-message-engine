#pragma once
#include "io_awaiter.hpp"
#include "io_context.hpp"
#include "mpsc_queue.hpp" // 替换为战役一的连续内存无锁队列
#include "task.hpp"
#include <algorithm>
#include <atomic>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

// CPU 亲和性绑核：防止 OS 调度器将核心线程迁走
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

// 引入 x86 硬件指令头文件
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

class CoroutineScheduler {
private:
  // 【核心引擎】：使用我们战役一打造的连续内存无锁 RingBuffer
  // 注意：容量必须是 2 的幂次方！(例如 1024, 65536)
  RingBufferMPSC<Task<int>> queue_;

  // io_uring 上下文，调度器的唯一 IO 引擎
  IoContext io_context_;

  // 控制调度器启停的原子开关，对齐缓存行防止伪共享
  alignas(64) std::atomic<bool> running_{false};

  // 绑定到当前调度器的唯一执行线程（Reactor 核心）
  std::thread worker_thread_;

  // Worker 绑定的 CPU 核心 ID（-1 = 不绑核）
  int worker_cpu_id_{-1};

  // 将指定线程绑定到某个 CPU 核心，使其彻底免疫 OS 的跨核迁移
  static void pin_thread_to_cpu(std::thread &t, int cpu_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
#endif
  }

public:
  // 初始化时指定环形队列的容量，强制 2 的幂次方对齐
  // worker_cpu_id: 将 Worker 绑定到哪个物理核心（-1 = 不绑核，交给 OS）
  explicit CoroutineScheduler(size_t queue_capacity = 65536,
                              int worker_cpu_id = -1,
                              unsigned int io_uring_entries = 1024)
      : queue_(queue_capacity), io_context_(io_uring_entries),
        worker_cpu_id_(worker_cpu_id) {}

  ~CoroutineScheduler() { stop(); }

  // 暴露 IoContext，供 IoAwaiter 提交任务时使用
  IoContext &get_io_context() { return io_context_; }

  void start(std::function<void()> on_worker_start = nullptr) {
    // 使用 release 语义，确保在此之前的初始化操作对 worker 线程可见
    running_.store(true, std::memory_order_release);

    // lambda 编译期判断是否是可调用对象
    // 编译器会生成一个匿名的类，重载 operator()
    // 运行时，先实例化lambda，然后创建线程，将lambda作为参数传递给线程，
    // 线程启动后，调用lambda的operator()方法
    worker_thread_ = std::thread([this, on_worker_start]() {
      if (on_worker_start) {
        on_worker_start();
      }
      this->run_loop();
    });

    // 【核心优化】：将 Worker 线程钉死在指定 CPU 核心上
    // 彻底消除 OS/Hypervisor 跨核迁移导致的缓存失效和调度抖动
    if (worker_cpu_id_ >= 0) {
      pin_thread_to_cpu(worker_thread_, worker_cpu_id_);
    }
  }

  void stop() {
    // 使用 acquire 检查当前状态
    if (running_.exchange(false, std::memory_order_acq_rel)) {
      if (worker_thread_.joinable()) {
        worker_thread_.join();
      }
    }
  }

  // 【极速入口】：前端业务线程（或其他 Worker）通过这里无锁投递任务
  bool submit(Task<int> task) {
    // 将 Task 的所有权彻底 Move 进无锁队列的槽位中
    if (queue_.push(std::move(task))) {
      io_context_.notify_wakeup();
      return true;
    }
    return false;
  }

private:
  // 【核心引擎】：单消费者死循环 (The Event Loop)
  void run_loop() {
    Task<int> current_task;
    size_t idle_spin_count = 0; // 空转计数器

    std::vector<Task<int>>
        active_tasks; // 任务活跃池，用于防止顶层协程挂起时被析构

    io_context_.prepare_wakeup_read();
    io_context_.submit();

    while (running_.load(std::memory_order_relaxed)) {
      if (!active_tasks.empty()) {
        active_tasks.erase(
            std::remove_if(active_tasks.begin(), active_tasks.end(),
                           [](const auto &t) { return t.get_handle().done(); }),
            active_tasks.end());
      }

      bool did_work = false;

      struct io_uring_cqe *cqe;
      while (io_context_.peek_cqe(&cqe) == 0) {
        did_work = true;
        void *user_data = io_uring_cqe_get_data(cqe);

        if (user_data == io_context_.wakeup_token_ptr()) {
          io_context_.cqe_seen(cqe);
          io_context_.prepare_wakeup_read();
        } else {
          IoAwaiter *awaiter = static_cast<IoAwaiter *>(user_data);
          if (awaiter) {
            awaiter->result_ = cqe->res;
            io_context_.cqe_seen(cqe);
            awaiter->handle_.resume();
          } else {
            io_context_.cqe_seen(cqe);
          }
        }
      }

      if (queue_.pop(current_task)) {
        did_work = true;
        if (current_task.resume()) {
          active_tasks.push_back(std::move(current_task));
        }
      }

      if (did_work) {
        idle_spin_count = 0;
        io_context_.submit();
      } else {
        idle_spin_count++;

        if (idle_spin_count < 1000) {
#if defined(__x86_64__) || defined(_M_X64)
          _mm_pause();
#endif
        } else if (idle_spin_count < 10000) {
          for (int i = 0; i < 10; ++i) {
#if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
#endif
          }
        } else {
          io_context_.submit();

          if (io_context_.wait_cqe(&cqe) == 0) {
            void *user_data = io_uring_cqe_get_data(cqe);
            if (user_data == io_context_.wakeup_token_ptr()) {
              io_context_.cqe_seen(cqe);
              io_context_.prepare_wakeup_read();
            } else {
              IoAwaiter *awaiter = static_cast<IoAwaiter *>(user_data);
              if (awaiter) {
                awaiter->result_ = cqe->res;
                io_context_.cqe_seen(cqe);
                awaiter->handle_.resume();
              } else {
                io_context_.cqe_seen(cqe);
              }
            }
            idle_spin_count = 0;
            io_context_.submit();
          }
        }
      }
    }

    while (queue_.pop(current_task)) {
      current_task.resume();
    }
  }
};