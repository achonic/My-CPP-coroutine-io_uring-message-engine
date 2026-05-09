#pragma once
#include <cassert>
#include <cstddef>
#include <cstdlib> // aligned_alloc / free
#include <new>     // std::bad_alloc
#include <vector>

#ifndef MYIO_ENABLE_CORO_POOL
#define MYIO_ENABLE_CORO_POOL 1
#endif

struct CoroutinePoolStats {
  std::size_t pool_allocations = 0;
  std::size_t fallback_allocations = 0;
  std::size_t pool_deallocations = 0;
  std::size_t fallback_deallocations = 0;
};

// ============================================================================
// 协程帧内存池 —— 固定帧大小 + 单所有者空闲链表（无锁、无 TLS）
//
// 设计原则：
//  本引擎是单线程事件循环架构。协程帧的 new/delete 只发生在 Worker 线程中。
//  外部生产者通过 MPSC 队列投递 Task 对象（轻量级 coroutine_handle 包装），
//  它们从不触及内存池。因此不需要 thread_local 也不需要任何锁。
//
// 优化策略：
//  - BLOCK_SIZE = 128B（协程帧大多数在128B以内，128B 对齐且浪费最小）
//  - 单所有者空闲链表：Worker 线程直接持有，零同步开销
//  - 按需扩容：4MB Slab 批量分配，减少系统调用
// ============================================================================

// 空闲块节点：复用空闲内存的前 8 字节存放 next 指针（侵入式链表）
struct PoolFreeBlock {
  PoolFreeBlock *next;
};

// 协程帧内存池 —— 单所有者设计
// 由 Worker 线程独占使用，不需要任何同步机制
class CoroutineMemoryPool {
  struct Slab {
    void *memory;
  };

  static constexpr std::size_t BLOCK_BYTES =
      128; // 覆盖常见协程帧(56~112B)，2×cache line 对齐
  static constexpr std::size_t SLAB_BYTES = 4 << 20; // 4MB
  static constexpr std::size_t BLOCKS_PER_SLAB =
      SLAB_BYTES / BLOCK_BYTES; // 32768

  // 空闲链表头指针（Worker 线程独占，无需原子操作）
  PoolFreeBlock *free_head_ = nullptr;

  // 空闲块计数（用于监控/调试，非必需）
  std::size_t free_count_ = 0;

  CoroutinePoolStats stats_{};

  // Slab 存储列表（用于析构时统一释放）
  std::vector<Slab> slabs_;

  // 扩容：
  // 1. 分配一块 4MB、64 字节对齐的大内存（Slab）
  // 2. 将 Slab 记录到 slabs_ 以便析构时统一释放
  // 3. 将 Slab 切分为 BLOCKS_PER_SLAB (32768) 个 128B 的 FreeBlock，
  //    逐个头插法插到空闲链表 free_head_ 中
  void grow() {
    void *raw = std::aligned_alloc(64, SLAB_BYTES);
    if (!raw)
      throw std::bad_alloc();
    slabs_.push_back({raw});

    // 把生内存转成char(1字节)，用来按字节偏移计算每个块的地址
    char *ptr = static_cast<char *>(raw);
    for (std::size_t i = 0; i < BLOCKS_PER_SLAB; ++i) {
      auto *block = reinterpret_cast<PoolFreeBlock *>(ptr + i * BLOCK_BYTES);
      block->next = free_head_;
      free_head_ = block;
    }
    free_count_ += BLOCKS_PER_SLAB;
  }

public:
  CoroutineMemoryPool() = default;

  // 析构函数：遍历所有已分配的 Slab，逐个 free 归还给操作系统。
  ~CoroutineMemoryPool() {
    for (auto &s : slabs_)
      std::free(s.memory);
  }

  // 禁止拷贝和移动（单例语义）
  CoroutineMemoryPool(const CoroutineMemoryPool &) = delete;
  CoroutineMemoryPool &operator=(const CoroutineMemoryPool &) = delete;

  // 返回单个内存块的字节大小（128B）
  static constexpr std::size_t block_size() { return BLOCK_BYTES; }

  // 分配一个协程帧内存块：
  // 1. 超大帧回退：若请求大小超过 BLOCK_BYTES (128B)，直接走全局 operator new
  // 2. 快路径：若空闲链表非空，O(1) 弹出头节点返回
  // 3. 慢路径：空闲链表耗尽，分配新的 4MB Slab 扩容，再弹出一个返回
  void *allocate(std::size_t size) {
#if !MYIO_ENABLE_CORO_POOL
    stats_.fallback_allocations++;
    return ::operator new(size);
#else
    if (size > BLOCK_BYTES) [[unlikely]] {
      stats_.fallback_allocations++;
      return ::operator new(size);
    }

    // 快路径：空闲链表有空闲块
    if (free_head_) [[likely]] {
      PoolFreeBlock *b = free_head_;
      free_head_ = b->next;
      free_count_--;
      stats_.pool_allocations++;
      return b;
    }

    // 慢路径：空闲链表空了，扩容
    grow();

    PoolFreeBlock *b = free_head_;
    free_head_ = b->next;
    free_count_--;
    stats_.pool_allocations++;
    return b;
#endif
  }

  // 释放一个协程帧内存块：
  // 1. 超大帧回退：若 size 超过 BLOCK_BYTES，走全局 operator delete
  // 2. 正常路径：将释放的块头插到空闲链表，O(1) 完成
  void deallocate(void *ptr, std::size_t size) {
#if !MYIO_ENABLE_CORO_POOL
    stats_.fallback_deallocations++;
    ::operator delete(ptr);
#else
    if (size > BLOCK_BYTES) [[unlikely]] {
      stats_.fallback_deallocations++;
      ::operator delete(ptr);
      return;
    }

    auto *b = static_cast<PoolFreeBlock *>(ptr);
    b->next = free_head_;
    free_head_ = b;
    free_count_++;
    stats_.pool_deallocations++;
#endif
  }

  // 获取当前空闲块数量（调试用）
  std::size_t free_count() const { return free_count_; }

  const CoroutinePoolStats &stats() const { return stats_; }
  void reset_stats() { stats_ = {}; }

  // 全局单例访问（保留 Meyers' Singleton，保证生命周期安全）
  // 虽然在单线程中 static 初始化的线程安全保证不是必需的，
  // 但 Singleton 模式本身解决的是生命周期问题，不是线程安全问题。
  static CoroutineMemoryPool &get_instance() {
    static CoroutineMemoryPool pool;
    return pool;
  }
};

// ============================================================================
// 内联分配/释放函数 —— 直接转发到全局单例
// ============================================================================

// 内存分配入口（由协程 promise_type::operator new 调用）
inline void *pool_allocate(std::size_t size) {
  return CoroutineMemoryPool::get_instance().allocate(size);
}

// 内存释放入口（由协程 promise_type::operator delete 调用）
inline void pool_deallocate(void *ptr, std::size_t size) {
  CoroutineMemoryPool::get_instance().deallocate(ptr, size);
}

inline const CoroutinePoolStats &pool_stats() {
  return CoroutineMemoryPool::get_instance().stats();
}

inline void reset_pool_stats() {
  CoroutineMemoryPool::get_instance().reset_stats();
}