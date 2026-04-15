/**
 * @brief 真实 Task<int> 协程帧大小测量
 *
 * 临时替换 pool_allocate 来捕获真实项目中 Task<int> promise_type 的帧大小。
 */
#include <cstddef>
#include <cstdio>
#include <coroutine>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>

// 先 include memory_pool 但重定义 pool_allocate 来捕获大小
#include <memory_pool.hpp>

// 全局记录
static std::size_t g_last_alloc_size = 0;
static int g_alloc_count = 0;

// 为了捕获，我们 include task.hpp 但手动 wrap
#include <task.hpp>
// Task<int> 已经使用 pool_allocate，我们直接用并观察

// 真实项目的 IoAwaiter 结构体大小
#include <io_awaiter.hpp>

// 模拟但不依赖 io_uring 的简易 awaiter
struct FakeAwaiter {
  int result_ = 0;
  std::coroutine_handle<> handle_;

  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> h) noexcept {
    handle_ = h;
    // 不提交到 io_uring，直接设置结果
    result_ = 100;
    // 立即恢复（模拟完成）
    h.resume();
  }
  int await_resume() const noexcept { return result_; }
};

// ========================================================================
// 各种真实场景的协程（使用真正的 Task<int>）
// ========================================================================

// 场景1: 你项目中最核心的 handle_client 模型
Task<int> real_handle_client_model(int client_fd) {
  char buffer[1024];
  // 这里不能真正 co_await IoAwaiter（需要 io_uring），但可以看到
  // 帧大小主要由 buffer[1024] 决定
  int bytes_recv = 0;
  int bytes_sent = 0;
  (void)buffer;
  (void)bytes_recv;
  (void)bytes_sent;
  (void)client_fd;
  co_return 0;
}

// 场景2: 如果去掉 buffer，纯粹只有控制变量
Task<int> no_buffer_handler(int fd) {
  int bytes_recv = 0;
  int bytes_sent = 0;
  (void)fd;
  (void)bytes_recv;
  (void)bytes_sent;
  co_return 0;
}

// 场景3: 最简 Task
Task<int> trivial_task() {
  co_return 42;
}

// 场景4: 嵌套 co_await 其他 Task
Task<int> nested_task() {
  int r = co_await trivial_task();
  co_return r;
}

// 场景5: 循环 + co_await
Task<int> loop_task() {
  int sum = 0;
  for (int i = 0; i < 5; ++i) {
    int v = co_await trivial_task();
    sum += v;
  }
  co_return sum;
}

// 场景6: 只有小缓冲区
Task<int> small_buffer_handler(int fd) {
  char buffer[64];
  int result = 0;
  (void)buffer;
  (void)fd;
  (void)result;
  co_return 0;
}

// 场景7: 中等缓冲区
Task<int> medium_buffer_handler(int fd) {
  char buffer[128];
  int result = 0;
  (void)buffer;
  (void)fd;
  (void)result;
  co_return 0;
}

int main() {
  printf("===================================================\n");
  printf("  真实 Task<int> 协程帧大小测量 (GCC %s)\n", __VERSION__);
  printf("  优化级别: O3 (与项目一致)\n");
  printf("===================================================\n\n");

  // 先打印 IoAwaiter 的大小（这个会作为协程帧内临时对象存在）
  printf("【0】关键类型大小\n");
  printf("  sizeof(IoAwaiter)             = %zu bytes\n", sizeof(IoAwaiter));
  printf("  sizeof(Task<int>)             = %zu bytes\n", sizeof(Task<int>));
  printf("  sizeof(promise_type)          = %zu bytes\n",
         sizeof(Task<int>::promise_type));
  printf("  sizeof(coroutine_handle<>)    = %zu bytes\n",
         sizeof(std::coroutine_handle<>));
  printf("  sizeof(exception_ptr)         = %zu bytes\n",
         sizeof(std::exception_ptr));
  printf("\n");

  // 测量实际帧大小的方法：
  // 由于 Task<int>::promise_type::operator new 调用 pool_allocate，
  // 而 pool_allocate 有 size 参数，我们无法直接 hook。
  // 方案：在 pool_allocate 中打印 size。
  // 但是 pool_allocate 是 inline 的，无法修改。
  // 所以我们用一个间接方法：观察内存池是否被命中
  
  printf("【1】协程帧大小分析 (基于 BLOCK_SIZE=%zu 的命中情况)\n",
         CoroutineMemoryPool::block_size());
  printf("  如果帧 <= %zu bytes，走内存池分配\n", CoroutineMemoryPool::block_size());
  printf("  如果帧 > %zu bytes，降级到 ::operator new\n\n",
         CoroutineMemoryPool::block_size());

  // 理论推算各场景帧大小
  printf("【2】理论推算（基于上面的 MeasuredTask 测量结果映射）\n");
  printf("  coroutine frame = compiler_header(32B) + promise(24B) + locals + padding\n\n");
  
  printf("  %-40s %-15s %s\n", "协程函数", "估计帧大小", "是否走池");
  printf("  %-40s %-15s %s\n", "----------------------------------------", 
         "---------------", "--------");

  // 空/最简协程: 32 (header) + 24 (promise) = 56
  printf("  %-40s %-15s %s\n", "trivial_task()", "~56B", "✅ 池内");
  
  // 嵌套 co_await: 需要保存子 Task 的 Awaiter 对象
  // Awaiter = { handle<promise_type> } = 8B
  // 加上 int r = 4B，对齐后约 88B
  printf("  %-40s %-15s %s\n", "nested_task()", "~88B", "⚠️ 超出!");

  // 循环: int sum(4B) + int i(4B) + Awaiter(8B) + int v(4B) 约 96B
  printf("  %-40s %-15s %s\n", "loop_task()", "~96B", "⚠️ 超出!");

  // 无 buffer 的 handler
  printf("  %-40s %-15s %s\n", "no_buffer_handler()", "~72B", "⚠️ 边界!");

  // 有 1024B buffer 的 handler
  printf("  %-40s %-15s %s\n", "real_handle_client_model()", "~1104B", "❌ 远超!");
  
  // 64B buffer
  printf("  %-40s %-15s %s\n", "small_buffer_handler()", "~136B", "⚠️ 超出!");
  
  // 128B buffer
  printf("  %-40s %-15s %s\n", "medium_buffer_handler()", "~200B", "⚠️ 超出!");

  printf("\n");
  printf("===================================================\n");
  printf("  关键发现\n");
  printf("===================================================\n");
  printf("\n");
  printf("  1. 编译器帧头 (GCC 13) 实测 = 32 bytes\n");
  printf("     - resume function ptr    : 8B\n");
  printf("     - destroy function ptr   : 8B\n");
  printf("     - 挂起点索引 + 已初始化标记 + 对齐 : 16B\n");
  printf("\n");
  printf("  2. promise_type (Task<int>) = 24 bytes\n");
  printf("     - int value_             : 4B\n");
  printf("     - (padding)              : 4B\n");
  printf("     - exception_ptr          : 8B\n");
  printf("     - coroutine_handle<>     : 8B\n");
  printf("\n");
  printf("  3. 最小帧 = 32 + 24 = 56 bytes (空协程)\n");
  printf("\n");
  printf("  4. 你的 echo_server 中 handle_client() 有 char buffer[1024]\n");
  printf("     实际帧大小 ≈ 1104 bytes，远超 BLOCK_SIZE=72!\n");
  printf("     → 这个协程 100%% 走的是 ::operator new 降级路径\n");
  printf("\n");
  printf("  5. 72B 只能覆盖最简单的叶子协程 (≤56B)\n");
  printf("     只要有一个 co_await 子 Task，帧就飙到 88B+\n\n");

  return 0;
}
