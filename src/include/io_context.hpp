#pragma once
#include <liburing.h>
#include <sys/eventfd.h>
#include <system_error>
#include <unistd.h>

/**
 * @brief IoContext 封装了 io_uring 的核心环形缓冲区操作。
 */
class IoContext {
public:
  static constexpr uintptr_t WAKEUP_TOKEN_VAL = 0xDEADBEEF;
  static void *wakeup_token_ptr() {
    return reinterpret_cast<void *>(WAKEUP_TOKEN_VAL);
  }

  // entries 是提交队列 (SQ) 的条目数量
  // CQ (Completion Queue) 的大小默认为 SQ 的 2 倍。
  explicit IoContext(unsigned int entries = 1024, unsigned int flags = 0) {
    int ret = io_uring_queue_init(entries, &ring_, flags);
    if (ret < 0) {
      throw std::system_error(-ret, std::generic_category(),
                              "io_uring_queue_init failed");
    }

    wakeup_fd_ = eventfd(0, EFD_NONBLOCK);
    if (wakeup_fd_ < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "eventfd creation failed");
    }
  }

  ~IoContext() {
    close(wakeup_fd_);
    io_uring_queue_exit(&ring_);
  }

  IoContext(const IoContext &) = delete;
  IoContext &operator=(const IoContext &) = delete;
  IoContext(IoContext &&) = delete;

  struct io_uring_sqe *get_sqe() {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
      submit();
      sqe = io_uring_get_sqe(&ring_);
    }
    return sqe;
  }

  int submit() {
    int ret = io_uring_submit(&ring_);
    if (ret < 0) {
      if (ret == -EBUSY)
        return 0;
      throw std::system_error(-ret, std::generic_category(),
                              "io_uring_submit failed");
    }
    return ret;
  }

  int wait_cqe(struct io_uring_cqe **cqe_ptr) {
    return io_uring_wait_cqe(&ring_, cqe_ptr);
  }

  int wait_cqe_timeout(struct io_uring_cqe **cqe_ptr, unsigned int timeout_ms) {
    struct __kernel_timespec ts;
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1000000LL;
    return io_uring_wait_cqe_timeout(&ring_, cqe_ptr, &ts);
  }

  int peek_cqe(struct io_uring_cqe **cqe_ptr) {
    return io_uring_peek_cqe(&ring_, cqe_ptr);
  }

  void cqe_seen(struct io_uring_cqe *cqe) { io_uring_cqe_seen(&ring_, cqe); }

  void prepare_wakeup_read() {
    struct io_uring_sqe *sqe = get_sqe();
    io_uring_prep_read(sqe, wakeup_fd_, &wakeup_buf_, sizeof(wakeup_buf_), 0);
    io_uring_sqe_set_data(sqe, wakeup_token_ptr());
  }

  void notify_wakeup() {
    uint64_t val = 1;
    ssize_t ret = write(wakeup_fd_, &val, sizeof(val));
    (void)ret;
  }

  struct io_uring *get_ring() { return &ring_; }

private:
  struct io_uring ring_;
  int wakeup_fd_;
  uint64_t wakeup_buf_;
};
