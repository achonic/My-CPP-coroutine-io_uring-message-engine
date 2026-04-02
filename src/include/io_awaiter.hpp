#pragma once
#include "io_context.hpp"
#include <coroutine>
#include <liburing.h>
#include <sys/socket.h>
#include <unistd.h>

/**
 * @brief IoAwaiter 是 io_uring 与 C++20 协程的桥接层。
 *
 * 参考 when_all.hpp 的设计：
 * 1. 它持有协程挂起后的上下文。
 * 2. await_suspend 负责将任务提交到 io_uring。
 * 3. 结果通过 await_resume 返回给协程。
 */
struct IoAwaiter {
  IoContext &ctx_;
  int fd_;
  void *buf_;
  size_t len_;
  off_t offset_;
  int opcode_;

  // 保存当前协程句柄，供 scheduler 唤醒
  std::coroutine_handle<> handle_;

  // 保存 IO 执行的结果（从 CQE 中获取）
  int result_ = 0;

  IoAwaiter(IoContext &ctx, int fd, void *buf, size_t len, off_t offset,
            int opcode)
      : ctx_(ctx), fd_(fd), buf_(buf), len_(len), offset_(offset),
        opcode_(opcode) {}

  // 总是挂起，等待 io_uring 完成通知
  bool await_ready() const noexcept { return false; }

  // 核心逻辑：将当前 Awaiter 实例绑定到 SQE，并提交给内核
  void await_suspend(std::coroutine_handle<> handle) noexcept {
    handle_ = handle;
    struct io_uring_sqe *sqe = ctx_.get_sqe();

    // 根据 opcode 初始化不同的 IO 操作( read/write/accpet )
    if (opcode_ == IORING_OP_READ) {
      io_uring_prep_read(sqe, fd_, buf_, len_, offset_);
    } else if (opcode_ == IORING_OP_WRITE) {
      io_uring_prep_write(sqe, fd_, buf_, len_, offset_);
    } else if (opcode_ == IORING_OP_ACCEPT) {
      io_uring_prep_accept(sqe, fd_, (struct sockaddr *)buf_, (socklen_t *)len_,
                           offset_);
    } else if (opcode_ == IORING_OP_RECV) {
      io_uring_prep_recv(sqe, fd_, buf_, len_, 0);
    } else if (opcode_ == IORING_OP_SEND) {
      io_uring_prep_send(sqe, fd_, buf_, len_, 0);
    }

    // 【关键】：将 Awaiter 实例指针存入 user_data
    // 因为 Awaiter 存在于协程栈帧中，其生命周期在协程挂起期间是安全的
    io_uring_sqe_set_data(sqe, this);

    // 提交任务交由调度器统一批处理 (任务 3.4)
    // ctx_.submit();
  }

  // 被唤醒后，返回 IO 操作的结果（如读写的字节数）
  int await_resume() const noexcept { return result_; }
};

/**
 * @brief 语义化的异步操作封装
 */
inline auto AsyncRead(IoContext &ctx, int fd, void *buf, size_t len,
                      off_t offset = 0) {
  return IoAwaiter(ctx, fd, buf, len, offset, IORING_OP_READ);
}

inline auto AsyncWrite(IoContext &ctx, int fd, const void *buf, size_t len,
                       off_t offset = 0) {
  return IoAwaiter(ctx, fd, const_cast<void *>(buf), len, offset,
                   IORING_OP_WRITE);
}

inline auto AsyncAccept(IoContext &ctx, int fd, struct sockaddr *addr,
                        socklen_t *addrlen) {
  // offset 参数在 accept 中对应 flags
  return IoAwaiter(ctx, fd, addr, (size_t)addrlen, 0, IORING_OP_ACCEPT);
}

inline auto AsyncRecv(IoContext &ctx, int fd, void *buf, size_t len) {
  return IoAwaiter(ctx, fd, buf, len, 0, IORING_OP_RECV);
}

inline auto AsyncSend(IoContext &ctx, int fd, const void *buf, size_t len) {
  return IoAwaiter(ctx, fd, const_cast<void *>(buf), len, 0, IORING_OP_SEND);
}
