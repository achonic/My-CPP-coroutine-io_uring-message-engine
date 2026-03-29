#pragma once

#include "mpsc_queue.hpp"
#include <atomic>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

// 基于 RingBufferMPSC 的高性能无锁异步日志引擎
// 目标：去除 std::cout 内部全局锁带来的竞争以及终端 I/O 对协程调度性能的影响
class AsyncLogger {
private:
  // 使用预先写好的大容量无锁队列（大小必须为 2
  // 的幂），避免终端消费不及时导致的阻塞现象
  RingBufferMPSC<std::string> queue_{65536};
  std::atomic<bool> running_{true};
  std::atomic<bool> enabled_{true}; // 新增日志全局开关
  std::thread worker_;

  AsyncLogger() {
    worker_ = std::thread([this]() {
      std::string msg;
      // 循环消费日志，保证极致的吞吐量
      while (running_.load(std::memory_order_acquire)) {
        if (queue_.pop(msg)) {
          std::cout << msg;
        } else {
          // 彻底解决空转峰值：不要用 pause 指令！它会导致 CPU 核心占用率 100%
          std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
      }

      // 停机阶段排干队列剩余的日志
      while (queue_.pop(msg)) {
        std::cout << msg;
      }
    });
  }

public:
  ~AsyncLogger() {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  // 禁用拷贝和赋值
  AsyncLogger(const AsyncLogger &) = delete;
  AsyncLogger &operator=(const AsyncLogger &) = delete;

  // 获取全局单例
  static AsyncLogger &getInstance() {
    static AsyncLogger instance;
    return instance;
  }

  // ==== 新增开关控制 API ====
  void setEnabled(bool enable) { enabled_.store(enable, std::memory_order_relaxed); }
  bool isEnabled() const { return enabled_.load(std::memory_order_relaxed); }

  // 极速全异步投递日志
  void log(std::string msg) {
    // 采用死循环推入，如果出现满列情况仅作让出保证日志不丢失
    while (!queue_.push(std::move(msg))) {
#if defined(__x86_64__) || defined(_M_X64)
      __builtin_ia32_pause();
#else
      std::this_thread::yield();
#endif
    }
  }
};

// 工具类，通过 RAII 将 operator<< 的流式输出收集起来，析构时一次性投入无锁队列
class LogMessage {
private:
  std::ostringstream oss_;

public:
  ~LogMessage() { AsyncLogger::getInstance().log(oss_.str()); }

  template <typename T> LogMessage &operator<<(const T &val) {
    oss_ << val;
    return *this;
  }

  // 特化 std::endl 处理，避免编译器找多态函数指针
  using endl_type = std::ostream &(std::ostream &);
  LogMessage &operator<<(endl_type val) {
    oss_ << val;
    return *this;
  }
};

// 用于替代 std::cout 的宏定义
// 使用 if (!cond) {} else 规避经典的悬挂 else (dangling-else) 问题
// 这样做还有一个好处：如果日志关闭，<< 后面的参数计算和流提取都会被短路跳过，实现真正的 0 性能损耗
#define ASYNC_LOG() if (!AsyncLogger::getInstance().isEnabled()) {} else LogMessage()
