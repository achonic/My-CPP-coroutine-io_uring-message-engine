#pragma once
#include <liburing.h>
#include <system_error>

/**
 * @brief IoContext 封装了 io_uring 的核心环形缓冲区操作。
 *
 * 符合 IO_URING_PLAN.md 阶段一的目标：
 * 1. 封装 io_uring_queue_init 和 io_uring_queue_exit。
 * 2. 提供 SQE (Submission Queue Entry) 的分配接口。
 */
class IoContext {
public:
  /**
   * @brief 初始化 io_uring
   * @param entries 提交队列的大小（必须是 2 的幂次方）
   * @param flags io_uring_setup 标志（如 IORING_SETUP_SQPOLL）
   */
  explicit IoContext(unsigned int entries = 1024, unsigned int flags = 0) {
    int ret = io_uring_queue_init(entries, &ring_, flags);
    if (ret < 0) {
      throw std::system_error(-ret, std::generic_category(),
                              "io_uring_queue_init failed");
    }
  }

  /**
   * @brief 优雅释放 io_uring 资源
   */
  ~IoContext() { io_uring_queue_exit(&ring_); }

  // 禁止拷贝
  IoContext(const IoContext &) = delete;
  IoContext &operator=(const IoContext &) = delete;

  // 目前暂不实现移动构造，因为 io_uring
  // 内部指针较多，通常作为单例或成员长期持有
  IoContext(IoContext &&) = delete;

  /**
   * @brief 获取一个提交队列项 (SQE)
   * 如果队列已满，会触发一次自动提交。
   * @return struct io_uring_sqe* 返回可用的 SQE 指针
   */
  struct io_uring_sqe *get_sqe() {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
      // 队列溢出时自动提交并重试
      submit();
      sqe = io_uring_get_sqe(&ring_);
    }
    return sqe;
  }

  /**
   * @brief 将 SQ 中的任务批量提交给内核
   * @return int 提交成功的任务数
   */
  int submit() {
    int ret = io_uring_submit(&ring_);
    if (ret < 0) {
      if (ret == -EBUSY)
        return 0; // 忽略忙错误
      throw std::system_error(-ret, std::generic_category(),
                              "io_uring_submit failed");
    }
    return ret;
  }

  /**
   * @brief 阻塞等待一个完成队列项 (CQE)
   * @param cqe_ptr 指向 CQE 指针的指针
   * @return int 状态码
   */
  int wait_cqe(struct io_uring_cqe **cqe_ptr) {
    return io_uring_wait_cqe(&ring_, cqe_ptr);
  }

  /**
   * @brief 非阻塞尝试获取一个完成队列项 (CQE)
   * @param cqe_ptr 指向 CQE 指针的指针
   * @return int 0 表示成功获取，-EAGAIN 表示队列为空
   */
  int peek_cqe(struct io_uring_cqe **cqe_ptr) {
    return io_uring_peek_cqe(&ring_, cqe_ptr);
  }

  /**
   * @brief 标记 CQE 已被处理
   * @param cqe 已处理的项
   */
  void cqe_seen(struct io_uring_cqe *cqe) { io_uring_cqe_seen(&ring_, cqe); }

  /**
   * @brief 获取底层句柄（用于高级操作）
   */
  struct io_uring *get_ring() { return &ring_; }

private:
  struct io_uring ring_;
};
