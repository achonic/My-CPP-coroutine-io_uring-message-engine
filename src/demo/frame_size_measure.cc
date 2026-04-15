/**
 * @brief 协程帧大小测量工具
 *
 * 通过 hook promise_type::operator new 来精确测量编译器为每个协程函数
 * 生成的帧（coroutine frame）大小。
 *
 * 编译命令：
 *   g++ -std=c++20 -O3 -march=native -I../include -o frame_size_measure
 * frame_size_measure.cc -luring
 */
#include <cstddef>
#include <cstdio>
#include <coroutine>
#include <cstring>
#include <exception>
#include <new>
#include <netinet/in.h>
#include <sys/socket.h>

// ============================================================================
// 1. 先定义一个带测量版 operator new 的 Task
// ============================================================================

// 全局变量：记录最近一次分配的帧大小
static std::size_t g_last_frame_size = 0;

template <typename T> class MeasuredTask {
public:
  struct promise_type {
    T value_;
    std::exception_ptr exception_;
    std::coroutine_handle<> continuation_ = nullptr;

    // 【测量核心】：记录编译器请求的帧大小
    void *operator new(std::size_t size) {
      g_last_frame_size = size;
      return ::operator new(size);
    }
    void operator delete(void *ptr, std::size_t size) {
      ::operator delete(ptr);
    }

    MeasuredTask get_return_object() {
      return MeasuredTask{
          std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
      bool await_ready() noexcept { return false; }
      template <typename PROMISE>
      std::coroutine_handle<>
      await_suspend(std::coroutine_handle<PROMISE> h) noexcept {
        auto &promise = h.promise();
        if (promise.continuation_)
          return promise.continuation_;
        return std::noop_coroutine();
      }
      void await_resume() noexcept {}
    };
    FinalAwaiter final_suspend() noexcept { return {}; }

    template <typename U> void return_value(U &&val) {
      value_ = std::forward<U>(val);
    }
    void unhandled_exception() { exception_ = std::current_exception(); }
  };

private:
  std::coroutine_handle<promise_type> handle_;
  explicit MeasuredTask(std::coroutine_handle<promise_type> h) : handle_(h) {}

public:
  MeasuredTask() noexcept : handle_(nullptr) {}
  MeasuredTask(const MeasuredTask &) = delete;
  MeasuredTask &operator=(const MeasuredTask &) = delete;
  MeasuredTask(MeasuredTask &&other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  MeasuredTask &operator=(MeasuredTask &&other) noexcept {
    if (this != &other) {
      if (handle_)
        handle_.destroy();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }
  ~MeasuredTask() {
    if (handle_)
      handle_.destroy();
  }

  auto operator co_await() noexcept {
    struct Awaiter {
      std::coroutine_handle<promise_type> handle_;
      bool await_ready() noexcept { return !handle_ || handle_.done(); }
      std::coroutine_handle<>
      await_suspend(std::coroutine_handle<> caller) noexcept {
        handle_.promise().continuation_ = caller;
        return handle_;
      }
      T await_resume() {
        if (handle_.promise().exception_)
          std::rethrow_exception(handle_.promise().exception_);
        return std::move(handle_.promise().value_);
      }
    };
    return Awaiter{handle_};
  }

  bool resume() {
    if (!handle_ || handle_.done())
      return false;
    handle_.resume();
    return !handle_.done();
  }
};

// ============================================================================
// 2. 模拟项目中各种协程函数，测量每个的帧大小
// ============================================================================

// --- Case 1: 最简协程，无局部变量 ---
MeasuredTask<int> minimal_coroutine() { co_return 42; }

// --- Case 2: 模拟 handle_client 中的局部变量 ---
// handle_client 有: char buffer[1024], 循环中的 int bytes_recv, int bytes_sent
// 以及隐含的: IoContext& ctx (引用=指针), int client_fd
MeasuredTask<int> simulated_handle_client(void *ctx_ptr, int client_fd) {
  char buffer[1024];
  // 模拟 co_await IoAwaiter（IoAwaiter 也要存在协程帧里）
  // IoAwaiter 的成员: IoContext& (8B), int fd(4B), void* buf(8B),
  //   size_t len(8B), off_t offset(8B), int opcode(4B),
  //   coroutine_handle<> handle(8B), int result(4B)
  // sizeof(IoAwaiter) ≈ 56B (含 padding)
  int bytes_recv = 0;
  int bytes_sent = 0;
  (void)ctx_ptr;
  (void)client_fd;
  (void)buffer;
  (void)bytes_recv;
  (void)bytes_sent;
  co_return 0;
}

// --- Case 3: 模拟 server_loop 中的局部变量 ---
// server_loop 有: IoContext& ctx (隐含已通过参数), sockaddr_in client_addr(16B),
//   socklen_t client_len(4B), int client_fd
MeasuredTask<int> simulated_server_loop(void *sched_ptr, int listen_fd) {
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  int client_fd = 0;
  (void)sched_ptr;
  (void)listen_fd;
  (void)client_addr;
  (void)client_len;
  (void)client_fd;
  co_return 0;
}

// --- Case 4: 单个 co_await 的简单协程 ---
MeasuredTask<int> single_await_coroutine() {
  auto inner = minimal_coroutine();
  int result = co_await inner;
  co_return result;
}

// --- Case 5: 多 co_await 的协程 ---
MeasuredTask<int> multi_await_coroutine() {
  auto a = minimal_coroutine();
  int r1 = co_await a;
  auto b = minimal_coroutine();
  int r2 = co_await b;
  co_return r1 + r2;
}

// --- Case 6: 带循环和简单变量的协程 ---
MeasuredTask<int> loop_coroutine() {
  int sum = 0;
  for (int i = 0; i < 10; ++i) {
    auto task = minimal_coroutine();
    int v = co_await task;
    sum += v;
  }
  co_return sum;
}

// --- Case 7: 纯粹什么都没有的协程 ---
MeasuredTask<int> empty_coroutine() { co_return 0; }

// --- Case 8: 模拟真实 echo handle_client 的全部变量（含 Awaiter-like 对象的大小估计） ---
// 真正的 handle_client co_await 表达式会在协程帧中暂存 IoAwaiter 临时对象
MeasuredTask<int> realistic_echo_handler(void *ctx, int fd) {
  char buffer[1024]; // 1024 bytes
  while (true) {
    // 每次 co_await 会在帧中为 IoAwaiter 的 awaiter 临时对象保留空间
    // IoAwaiter 内存布局 (x86-64):
    //   IoContext&  ctx_     : 8 bytes (引用=指针)
    //   int         fd_      : 4 bytes
    //   (padding)            : 4 bytes
    //   void*       buf_     : 8 bytes
    //   size_t      len_     : 8 bytes
    //   off_t       offset_  : 8 bytes
    //   int         opcode_  : 4 bytes
    //   (padding)            : 4 bytes
    //   handle<>    handle_  : 8 bytes
    //   int         result_  : 4 bytes
    //   (padding)            : 4 bytes
    // total IoAwaiter = 64 bytes

    int bytes_recv = 0;
    int bytes_sent = 0;
    (void)buffer;
    (void)bytes_recv;
    (void)bytes_sent;
    (void)ctx;
    (void)fd;
    co_return 0; // break out immediately for measurement
  }
}

// ============================================================================
// 3. promise_type 本身的大小分析
// ============================================================================

struct PromiseSizeInfo {
  std::size_t value_size;
  std::size_t exception_ptr_size;
  std::size_t handle_size;
  std::size_t total_promise_size;
};

PromiseSizeInfo get_promise_sizes() {
  using P = MeasuredTask<int>::promise_type;
  return {
      sizeof(P::value_),
      sizeof(std::exception_ptr),
      sizeof(std::coroutine_handle<>),
      sizeof(P),
  };
}

// ============================================================================
// Main
// ============================================================================
int main() {
  printf("===================================================\n");
  printf("  C++20 协程帧大小精确测量报告\n");
  printf("===================================================\n\n");

  // --- promise_type 分析 ---
  auto pinfo = get_promise_sizes();
  printf("【1】promise_type 内存布局分析\n");
  printf("  sizeof(T value_)              = %zu bytes\n", pinfo.value_size);
  printf("  sizeof(exception_ptr)         = %zu bytes\n",
         pinfo.exception_ptr_size);
  printf("  sizeof(coroutine_handle<>)    = %zu bytes\n", pinfo.handle_size);
  printf("  sizeof(promise_type) [total]  = %zu bytes\n",
         pinfo.total_promise_size);
  printf("\n");

  // --- 编译器内部帧管理头部估算 ---
  printf("【2】编译器帧管理头部 (coroutine frame header)\n");
  printf("  编译器 ABI 帧头通常包含:\n");
  printf("    - resume function pointer   : 8 bytes\n");
  printf("    - destroy function pointer  : 8 bytes\n");
  printf("    - suspension point index    : 2~4 bytes (通常对齐到 8)\n");
  printf("    - (optional) padding        : varies\n");
  printf("  GCC/Clang 典型帧头             ≈ 16~24 bytes\n\n");

  // --- 各协程的实际帧大小 ---
  printf("【3】各协程函数的实际帧大小 (operator new 收到的 size)\n");
  printf("  %-40s %s\n", "协程函数", "帧大小 (bytes)");
  printf("  %-40s %s\n", "----------------------------------------",
         "--------------");

  // 测量每个协程
  auto measure = [](const char *name, auto coroutine_fn) {
    g_last_frame_size = 0;
    auto task = coroutine_fn();
    printf("  %-40s %zu\n", name, g_last_frame_size);
    return g_last_frame_size;
  };

  auto s1 = measure("empty_coroutine()", [] { return empty_coroutine(); });
  auto s2 = measure("minimal_coroutine()", [] { return minimal_coroutine(); });
  auto s3 = measure("single_await_coroutine()",
                     [] { return single_await_coroutine(); });
  auto s4 = measure("multi_await_coroutine()",
                     [] { return multi_await_coroutine(); });
  auto s5 =
      measure("loop_coroutine()", [] { return loop_coroutine(); });
  auto s6 = measure("simulated_handle_client()",
                     [] { return simulated_handle_client(nullptr, 5); });
  auto s7 = measure("simulated_server_loop()",
                     [] { return simulated_server_loop(nullptr, 5); });
  auto s8 = measure("realistic_echo_handler()",
                     [] { return realistic_echo_handler(nullptr, 5); });

  printf("\n");

  // --- 超过 BLOCK_SIZE 的风险分析 ---
  constexpr std::size_t BLOCK_SIZE = 72;
  printf("【4】内存池 BLOCK_SIZE=%zu 的适配性分析\n", BLOCK_SIZE);

  auto check = [](const char *name, std::size_t frame_size,
                   std::size_t block_size) {
    if (frame_size <= block_size) {
      printf("  ✅ %-35s %zu ≤ %zu  → 池内分配\n", name, frame_size,
             block_size);
    } else {
      printf("  ⚠️  %-35s %zu > %zu  → 降级到 ::operator new!\n", name,
             frame_size, block_size);
    }
  };

  check("empty_coroutine", s1, BLOCK_SIZE);
  check("minimal_coroutine", s2, BLOCK_SIZE);
  check("single_await_coroutine", s3, BLOCK_SIZE);
  check("multi_await_coroutine", s4, BLOCK_SIZE);
  check("loop_coroutine", s5, BLOCK_SIZE);
  check("simulated_handle_client", s6, BLOCK_SIZE);
  check("simulated_server_loop", s7, BLOCK_SIZE);
  check("realistic_echo_handler", s8, BLOCK_SIZE);

  printf("\n");

  // --- 帧大小的理论分解 ---
  printf("【5】协程帧大小的理论分解公式\n");
  printf("  coroutine_frame_size = \n");
  printf("      compiler_header (≈16~24B)\n");
  printf("    + sizeof(promise_type) (%zuB)\n", pinfo.total_promise_size);
  printf("    + local_variables_size\n");
  printf("    + max(sizeof(awaiter₁), sizeof(awaiter₂), ...) (awaiter 临时对象共用空间)\n");
  printf("    + alignment_padding\n");
  printf("\n");
  printf("  对于你的 empty_coroutine:\n");
  printf("    compiler_header + promise(%zu) + 0 locals + padding = %zu\n",
         pinfo.total_promise_size, s1);
  printf("    → 推算 compiler_header ≈ %zu bytes\n",
         s1 - pinfo.total_promise_size);

  printf("\n===================================================\n");
  printf("  测量完成\n");
  printf("===================================================\n");

  return 0;
}
