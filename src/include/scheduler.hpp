#pragma once
#include "mpsc_queue.hpp" // 替换为战役一的连续内存无锁队列
#include "task.hpp"
#include <algorithm>
#include <atomic>
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
                              int worker_cpu_id = -1)
      : queue_(queue_capacity), worker_cpu_id_(worker_cpu_id) {}

  ~CoroutineScheduler() { stop(); }

  void start() {
    // 使用 release 语义，确保在此之前的初始化操作对 worker 线程可见
    running_.store(true, std::memory_order_release);

    // lambda 编译期判断是否是可调用对象
    // 编译器会生成一个匿名的类，重载 operator()
    // 运行时，先实例化lambda，然后创建线程，将lambda作为参数传递给线程，
    // 线程启动后，调用lambda的operator()方法
    worker_thread_ = std::thread([this]() { this->run_loop(); });

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
        } else {
          // 协程已经结束，可以安全销毁
          // current_task 此时持有 handle 并在本轮迭代末尾析构
        }
      } else {
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
          // 阶段
          // 3：真正的真空期。只有在长时间没有任务时，才极其吝啬地让出时间片。
          // 此时系统处于绝对空闲，让出 CPU 给网络 I/O 轮询线程是合理的。
          std::this_thread::yield();
          // 在纯粹的极速场景中，哪怕到了这里也只用 _mm_pause，宁可 CPU 100%
          // 空转。
        }
      }
    }

    // 调度器被 stop 后，优雅地清空队列里的残余任务
    while (queue_.pop(current_task)) {
      current_task.resume();
    }
  }
};