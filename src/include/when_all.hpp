#pragma once
#include "scheduler.hpp"
#include "task.hpp"
#include <atomic>
#include <utility>

class WhenAllAwaiter {
private:
  // 暂存原始任务
  Task<int> t1_;
  Task<int> t2_;
  CoroutineScheduler &orch_;

  // 【核心控制块】：被外层协程的栈帧持有，生命周期与外层 co_await 严格绑定
  std::atomic<int> count_{2};
  std::coroutine_handle<> continuation_{nullptr};

  // 结果收敛区
  int res1_{0};
  int res2_{0};

public:
  WhenAllAwaiter(CoroutineScheduler &orch, Task<int> &&t1, Task<int> &&t2)
      : orch_(orch), t1_(std::move(t1)), t2_(std::move(t2)) {}

  // 永远挂起外层协程，因为我们要等待 A 和 B 并发跑完
  bool await_ready() const noexcept { return false; }

  // 当外层协程被挂起时触发，caller 是外层协程的“遥控器”
  void await_suspend(std::coroutine_handle<> caller) noexcept {
    continuation_ = caller;

    // 生成两个 Helper 包装任务，把它们扔进引擎去并发执行。
    // 注意：这里必须把 count_ 的指针传进去，因为当最后一个任务执行 resume()
    // 后， 外层协程可能立刻结束并销毁这个 Awaiter，导致 this 失效。
    orch_.submit(launch_wrapper(std::move(t1_), res1_, &count_, continuation_));
    orch_.submit(launch_wrapper(std::move(t2_), res2_, &count_, continuation_));
  }

  // 外层协程被重新唤醒时，拿走收敛好的结果
  std::pair<int, int> await_resume() noexcept { return {res1_, res2_}; }

private:
  // Helper 包装协程：负责执行子任务并在结束时原子扣减计数
  static Task<int> launch_wrapper(Task<int> target_task, int &out_res,
                                  std::atomic<int> *count_ptr,
                                  std::coroutine_handle<> cont) {
    // 等待真实的业务逻辑跑完
    out_res = co_await target_task;

    // 【架构核心】：无冲突唤醒与内存序屏障
    // fetch_sub 返回扣减前的值。如果旧值是 1，说明当前线程是最后一个执行完的！
    if (count_ptr->fetch_sub(1, std::memory_order_acq_rel) == 1) {
      // 拿到接力棒，唤醒等待在 WhenAll 上的父协程！
      // 在对称传输架构下，这一步不会陷入内核，而是直接在当前 CPU
      // 核心上极速恢复业务流
      cont.resume();
    }

    co_return 0; // 这里的返回值不重要了，因为真实结果已经写入 out_res
  }
};

// 优雅的工厂函数，方便业务侧调用
inline WhenAllAwaiter WhenAll(CoroutineScheduler &orch, Task<int> t1,
                              Task<int> t2) {
  return WhenAllAwaiter(orch, std::move(t1), std::move(t2));
}