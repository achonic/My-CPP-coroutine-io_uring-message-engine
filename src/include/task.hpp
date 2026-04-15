#pragma once
#include <coroutine>
#include <memory_pool.hpp>
#include <stdexcept>
#include <utility>

// #include <new> // 用于 placement new 和内存池

template <typename T> class Task {
public:
  struct promise_type {
    T value_;
    std::exception_ptr exception_;

    // 【核心新增】：Continuations 机制
    // 记住“是谁在等待我完成”。当本协程执行完毕后，需要唤醒这个 continuation_。
    std::coroutine_handle<> continuation_ = nullptr;

    // --- 内存池 ---
    // 通过内存池分配，减少系统调用次数和指令数。
    void *operator new(std::size_t size) { return pool_allocate(size); }
    void operator delete(void *ptr, std::size_t size) {
      pool_deallocate(ptr, size);
    }

    // 协程构造完成后，调用此函数，返回 Task 对象（由编译器自动调用）
    // 拿到只有编译器知道的 promise_type 句柄
    Task get_return_object() {
      return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    // 协程开始执行前，调用此函数
    // 返回 suspend_always 表示协程一创建就挂起，等待第一次 resume
    std::suspend_always initial_suspend() noexcept { return {}; }

    // 【核心改造】：final_suspend 与对称传输
    // 协程结束，co_return ，不再只是挂起，而是直接通过底层硬件寄存器跳转到
    // continuation_ 协程 co_await 对象 三件套await_ready()， await_suspend() ，
    // await_resume()
    struct FinalAwaiter {
      bool await_ready() noexcept { return false; }

      template <typename PROMISE>
      std::coroutine_handle<>
      await_suspend(std::coroutine_handle<PROMISE> h) noexcept {
        auto &promise = h.promise();
        if (promise.continuation_) {
          // 对称传输：直接返回等待者的句柄。
          // 编译器会在用户态执行类似 JMP 的跳转，彻底避免互相 resume()
          // 导致的爆栈！

          // 非对称传输的话，会发生连环 resume，导致栈溢出
          // 因为 resume()
          // 只是一个函数调用，会把当前栈帧压栈，然后执行下一个协程
          // 栈帧没有消失，如果A co_await B, B co_await C, C co_await D, D
          // co_await E
          // 那么当E执行完毕后，会返回D的栈帧，D执行完毕后，会返回C的栈帧，C执行完毕后，会返回B的栈帧，B执行完毕后，会返回A的栈帧
          // 每一个栈帧都没有被销毁，如果协程数量非常多，就会导致栈溢出
          return promise.continuation_;
        }
        // 如果没有等待者（比如它是顶层任务），则返回 noop 挂起，等待调度器回收
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

  explicit Task(std::coroutine_handle<promise_type> h) : handle_(h) {}

public:
  Task() noexcept : handle_(nullptr) {}

  // --- 严格的 Move-only 语义 ---
  Task(const Task &) = delete;
  Task &operator=(const Task &) = delete;

  Task(Task &&other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }

  Task &operator=(Task &&other) noexcept {
    if (this != &other) {
      if (handle_)
        handle_.destroy();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  ~Task() {
    if (handle_) {
      handle_.destroy();
    }
  }

  // 【核心新增】：让 Task 本身成为可被 co_await 的对象
  // 使得你可以写出 auto res = co_await SubTask(); 这样的优雅代码
  // C++20 ISO/IEC 14882:2020 规定的 operator co_await 运算符重载
  auto operator co_await() noexcept {
    struct Awaiter {
      std::coroutine_handle<promise_type> handle_;

      // 询问需不需要挂起，如果任务已经跑完了，就不需要挂起了
      bool await_ready() noexcept { return !handle_ || handle_.done(); }

      // caller 是正在执行 co_await 的外层协程
      std::coroutine_handle<>
      await_suspend(std::coroutine_handle<> caller) noexcept {
        // 将外层协程的句柄保存到子任务的 continuation_ 中
        // C++20标准库<coroutine>规定有一个成员函数
        // promise()返回promise_type的引用，然后就可以调用promise_type的成员函数了
        handle_.promise().continuation_ = caller;

        // 返回子任务的句柄，触发对称传输，开始执行子任务！
        return handle_;
      }

      // 当子任务完成并跳转回来后，提取返回值给外层协程
      T await_resume() {
        if (handle_.promise().exception_) {
          std::rethrow_exception(handle_.promise().exception_);
        }
        return std::move(handle_.promise().value_);
      }
    };

    return Awaiter{handle_};
  }

  // --- 调度器暴露的顶层控制接口 ---
  bool resume() {
    if (!handle_ || handle_.done())
      return false;
    handle_.resume();
    return !handle_.done();
  }

  T get_result() {
    if (!handle_.done())
      throw std::logic_error("Task is not finished yet!");
    if (handle_.promise().exception_)
      std::rethrow_exception(handle_.promise().exception_);
    return handle_.promise().value_;
  }

  std::coroutine_handle<promise_type> get_handle() const { return handle_; }
};