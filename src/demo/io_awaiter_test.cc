#include "io_awaiter.hpp"
#include "task.hpp"
#include <cstring>
#include <fcntl.h>
#include <iostream>

/**
 * @brief 测试协程：演示如何使用 AsyncRead
 */
Task<int> test_async_read(IoContext &ctx, int fd) {
  char buffer[128] = {0};
  std::cout << "[Coroutine] Starting AsyncRead..." << std::endl;

  // 关键点：这里会触发 IoAwaiter::await_suspend
  // AsyncRead 返回 IoAwaiter 对象，co_await 会调用其 await_suspend
  int bytes_read = co_await AsyncRead(ctx, fd, buffer, sizeof(buffer) - 1);

  if (bytes_read >= 0) {
    buffer[bytes_read] = '\0';
    std::cout << "[Coroutine] Read success, bytes: " << bytes_read << std::endl;
    std::cout << "[Coroutine] Data: " << buffer << std::endl;
  } else {
    std::cerr << "[Coroutine] Read failed with error: " << bytes_read
              << std::endl;
  }

  co_return bytes_read;
}

int main() {
  const char *filename = "test_io.txt";
  int fd_write = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  const char *data = "Hello io_uring Bridge with Awaiter!";
  write(fd_write, data, strlen(data));
  close(fd_write);

  try {
    // 设置环的大小， 初始化 io_uring，拿到ring_句柄
    IoContext ctx(64);
    // 打开文件 只读
    int fd_read = open(filename, O_RDONLY);

    // 在堆上开辟一块内存，用于存储协程对象（协程帧）
    // 在协程帧里构造 Task<int>::promise_type 对象
    // 调用 promise.get_return_object()。
    // 这个调用会创建 Task<int> 对象。
    // 进入协程体，立刻遇到 co_await promise.initial_suspend()。
    // 由于 initial_suspend 返回 suspend_always，所以协程立即挂起
    auto t = test_async_read(ctx, fd_read);
    // 第一次 resume，执行到 co_await AsyncRead(...)
    // 调用 AsyncRead 的 await_suspend 方法
    t.resume();

    std::cout << "[Main] Submitting and Waiting for CQE..." << std::endl;
    ctx.submit(); // 【关键修复】：由于 IoAwaiter 不再自动 submit，这里需要手动提交

    struct io_uring_cqe *cqe;
    int ret = ctx.wait_cqe(&cqe);
    if (ret == 0) {
      // 从 user_data 还原 IoAwaiter 指针
      IoAwaiter *awaiter = static_cast<IoAwaiter *>(io_uring_cqe_get_data(cqe));

      // 填回结果
      awaiter->result_ = cqe->res;

      char *data = static_cast<char *>(awaiter->buf_);
      data[awaiter->result_] = '\0';

      std::cout << "[Main] IO Completed, res: " << cqe->res << std::endl;
      std::cout << "[Main] Data: " << data << std::endl;
      // 唤醒协程
      awaiter->handle_.resume();

      ctx.cqe_seen(cqe);
    }

    close(fd_read);
    unlink(filename);

  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
