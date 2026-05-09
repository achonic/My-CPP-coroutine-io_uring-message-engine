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
    return queue_.push(std::move(task));
  }

private:
  // 【核心引擎】：单消费者死循环 (The Event Loop)
  void run_loop() {
    Task<int> current_task;
    size_t idle_spin_count = 0; // 空转计数器

    std::vector<Task<int>>
        active_tasks; // 任务活跃池，用于防止顶层协程挂起时被析构

    while (running_.load(std::memory_order_relaxed)) {
      // 检查是否有已经完成的任务需要回收
      if (!active_tasks.empty()) {
        // erase-remove_if 的用法
        // remove_if 把已经done的任务移到vector的末尾，返回新的尾部迭代器
        // erase 删除从新尾部迭代器到原末尾的所有元素
        active_tasks.erase(
            std::remove_if(active_tasks.begin(), active_tasks.end(),
                           [](const auto &t) { return t.get_handle().done(); }),
            active_tasks.end());
      }

      // 尝试从无锁队列中剥离出一个协程任务
      if (queue_.pop(current_task)) {
        idle_spin_count = 0; // 拿到任务，重置空转计数

        // 唤醒底层的协程状态机
        if (current_task.resume()) {
          // 如果协程挂起（未结束），将其移交给 active_tasks 维持生命周期
          active_tasks.push_back(std::move(current_task));
        }

        // 重要：在处理完业务协程逻辑后，如果有新的 IO SQE 被填充，在这里 flush
        // 进内核
        io_context_.submit();

      } else {
        // 尝试从 io_uring 收割完成的 IO 事件 (策略 A & 任务 3.3)
        struct io_uring_cqe *cqe;
        bool has_io = false;
        while (io_context_.peek_cqe(&cqe) == 0) {
          has_io = true;
          IoAwaiter *awaiter =
              static_cast<IoAwaiter *>(io_uring_cqe_get_data(cqe));
          if (awaiter) {
            awaiter->result_ = cqe->res;
            io_context_.cqe_seen(cqe);
            awaiter->handle_.resume();
          } else {
            io_context_.cqe_seen(cqe);
          }
        }

        if (has_io) {
          idle_spin_count = 0;
          io_context_.submit(); // 确保刚产生的新 IO 进入内核
          continue; // 只要有 IO 完成，就认为系统在忙碌，继续下一轮循环
        }

        // 没有任务就自增空转计数
        // 【关键改造】：自适应退避策略 (Exponential Backoff)
        // 遇到真空期，绝对不能调用 yield() 陷入内核态！
        idle_spin_count++;

        if (idle_spin_count < 1000) {
// 阶段 1：极度活跃期。只调用硬件级的 pause 指令。
// 这会让 CPU 稍微歇一口气，清空流水线，但不会触发线程切换。
#if defined(__x86_64__) || defined(_M_X64)
          _mm_pause();
#endif
        } else if (idle_spin_count < 10000) {
          // 阶段 2：轻度空闲期。执行多次 pause，进一步降低总线占用。
          for (int i = 0; i < 10; ++i) {
#if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
#endif
          }
        } else {
          // 阶段 3：真正的真空期。既没有计算任务，也没有立即可收割的 IO。
          // 此时调用阻塞的 io_context.wait_cqe 沉睡，让出核心。
          io_context_
              .submit(); // 阻塞前确保提交所有堆积的 IO 请求 (任务 3.4 准备)

          if (io_context_.wait_cqe(&cqe) == 0) {
            IoAwaiter *awaiter =
                static_cast<IoAwaiter *>(io_uring_cqe_get_data(cqe));
            if (awaiter) {
              awaiter->result_ = cqe->res;
              io_context_.cqe_seen(cqe);
              awaiter->handle_.resume();
            } else {
              io_context_.cqe_seen(cqe);
            }
            idle_spin_count = 0;
          }
        }
      }
    }

    // 调度器被 stop 后，优雅地清空队列里的残余任务
    while (queue_.pop(current_task)) {
      current_task.resume();
    }
  }
};