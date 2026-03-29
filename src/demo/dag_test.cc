#include "async_logger.hpp"
#include "scheduler.hpp"
#include "task.hpp"
#include "when_all.hpp"
#include <atomic>
#include <utility>

// 用于通知主线程业务流已全部跑完
std::atomic<bool> global_done{false};
std::atomic<int> final_result{0};

// 子任务 A：模拟查数据库
Task<int> fetch_db_data(int id) {
  ASYNC_LOG() << "[Thread " << std::this_thread::get_id()
              << "] 🟢 DB Task: 开始查询数据库, ID=" << id << "...\n";
  // 模拟一段极速的 CPU 计算（真实场景下这里可能是 co_await io_uring_read）
  int res = id * 10;
  ASYNC_LOG() << "[Thread " << std::this_thread::get_id()
              << "] 🟢 DB Task: 查询完成, 返回 " << res << "\n";
  co_return res;
}

// 子任务 B：模拟查缓存
Task<int> fetch_cache_data(int id) {
  ASYNC_LOG() << "[Thread " << std::this_thread::get_id()
              << "] 🟡 Cache Task: 开始查询缓存, ID=" << id << "...\n";
  int res = id * 100;
  ASYNC_LOG() << "[Thread " << std::this_thread::get_id()
              << "] 🟡 Cache Task: 查询完成, 返回 " << res << "\n";
  co_return res;
}

// 顶层主任务：测试 DAG 依赖流转
Task<int> complex_business_logic(CoroutineScheduler &orch, int query_id) {
  ASYNC_LOG() << "[Thread " << std::this_thread::get_id()
              << "] 🚀 Main Task: 业务流启动，准备并行下发子任务...\n";

  // 【架构高光时刻】：主协程在此处彻底交出 CPU 控制权，并挂起自己！
  // DB 和 Cache 任务会被 push 进无锁队列，由调度器接管并发执行。
  auto result_pair = co_await WhenAll(orch, fetch_db_data(query_id),
                                      fetch_cache_data(query_id));

  // 只有当 DB 和 Cache 都 co_return
  // 之后，最后完成的那个任务才会通过对称传输把这里唤醒
  int total = result_pair.first + result_pair.second;
  ASYNC_LOG() << "[Thread " << std::this_thread::get_id()
              << "] 🎯 Main Task: 依赖任务全部收敛！聚合结果: " << total
              << "\n";

  // 写入最终结果，并使用 release 保证数据对主线程可见
  final_result.store(total, std::memory_order_relaxed);
  global_done.store(true, std::memory_order_release);

  co_return total;
}

int main() {
  ASYNC_LOG() << "========== DAG Orchestration Test ==========\n";

  // 初始化容量为 1024 的连续内存无锁调度器
  CoroutineScheduler scheduler(1024);

  scheduler.start();

  // 业务线程：把顶层任务以无锁、零拷贝的方式砸进队列
  scheduler.submit(complex_business_logic(scheduler, 42));

  // 主线程自旋等待顶层任务完成
  // 严格使用 acquire 做向下屏蔽！
  // 避免因为指令重排，导致 !global_done 的判断在 load
  // 真正拿到内存值之前就提前执行。 如果重排发生，进入了循环体才 load 出
  // true，就会白白执行一次 _mm_pause() 或 yield()，增加无谓的性能损耗。
  while (!global_done.load(std::memory_order_acquire)) {
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
  }

  ASYNC_LOG() << "========================================================\n";
  ASYNC_LOG() << "[Main Thread] 测试圆满结束！最终收敛结果: "
              << final_result.load(std::memory_order_relaxed) << "\n";

  scheduler.stop();
  return 0;
}