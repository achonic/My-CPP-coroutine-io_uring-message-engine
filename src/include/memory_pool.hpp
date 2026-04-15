#pragma once
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib> // aligned_alloc / free
#include <new>     // std::bad_alloc
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

// ============================================================================
// 极简自旋锁
// ============================================================================
class SpinLock {
  std::atomic_flag flag_ = ATOMIC_FLAG_INIT;

public:
  void lock() noexcept {
    while (flag_.test_and_set(std::memory_order_acquire)) {
#if defined(__x86_64__) || defined(_M_X64)
      _mm_pause();
#endif
    }
  }
  void unlock() noexcept { flag_.clear(std::memory_order_release); }
};

class SpinLockGuard {
  SpinLock &lock_;

public:
  explicit SpinLockGuard(SpinLock &lock) : lock_(lock) { lock_.lock(); }
  ~SpinLockGuard() { lock_.unlock(); }
  SpinLockGuard(const SpinLockGuard &) = delete;
  SpinLockGuard &operator=(const SpinLockGuard &) = delete;
};

// ============================================================================
// 协程帧内存池 —— 固定帧大小 + thread_local 裸指针缓存
//
// 优化策略：
//  - BLOCK_SIZE = 128B（协程帧实测 72B，128B 对齐且浪费最小）
//  - 使用 __thread 裸变量代替 static thread_local class，
//    避免 __tls_get_addr() 函数调用开销（GCC 动态 TLS 模型）
//  - 全局池仅在批量补货/归还时触发自旋锁
// ============================================================================

// 使用裸 FreeBlock 指针做 thread_local 缓存，避免 static thread_local 的
// __tls_get_addr 开销。GCC 对 POD 类型的 __thread 使用更快的 initial-exec TLS
// 模型。
struct PoolFreeBlock {
  PoolFreeBlock *next;
};

// 全局 Slab 管理器
class CoroutineMemoryPool {
  struct Slab {
    void *memory;
  };

  static constexpr std::size_t BLOCK_BYTES = 128; // 覆盖常见协程帧(56~112B)，2×cache line 对齐
  static constexpr std::size_t SLAB_BYTES = 4 << 20; // 4MB
  static constexpr std::size_t BLOCKS_PER_SLAB =
      SLAB_BYTES / BLOCK_BYTES; // 32768
  static constexpr std::size_t REFILL_BATCH = 4096;

  SpinLock lock_;
  PoolFreeBlock *global_free_ = nullptr;
  std::vector<Slab> slabs_;

  void grow_locked() {
    void *raw = std::aligned_alloc(64, SLAB_BYTES);
    if (!raw)
      throw std::bad_alloc();
    slabs_.push_back({raw});

    char *ptr = static_cast<char *>(raw);
    for (std::size_t i = 0; i < BLOCKS_PER_SLAB; ++i) {
      auto *block = reinterpret_cast<PoolFreeBlock *>(ptr + i * BLOCK_BYTES);
      block->next = global_free_;
      global_free_ = block;
    }
  }

  PoolFreeBlock *take_batch_locked(std::size_t count) {
    if (!global_free_)
      grow_locked();
    PoolFreeBlock *batch = global_free_;
    PoolFreeBlock *tail = batch;
    for (std::size_t i = 1; i < count && tail->next; ++i)
      tail = tail->next;
    global_free_ = tail->next;
    tail->next = nullptr;
    return batch;
  }

  void return_batch_locked(PoolFreeBlock *head, PoolFreeBlock *tail) {
    tail->next = global_free_;
    global_free_ = head;
  }

public:
  CoroutineMemoryPool() = default;
  ~CoroutineMemoryPool() {
    for (auto &s : slabs_)
      std::free(s.memory);
  }

  static constexpr std::size_t block_size() { return BLOCK_BYTES; }
  static constexpr std::size_t refill_batch() { return REFILL_BATCH; }

  PoolFreeBlock *refill(std::size_t count) {
    SpinLockGuard guard(lock_);
    return take_batch_locked(count);
  }

  void return_blocks(PoolFreeBlock *head, PoolFreeBlock *tail) {
    SpinLockGuard guard(lock_);
    return_batch_locked(head, tail);
  }

  static CoroutineMemoryPool &get_instance() {
    static CoroutineMemoryPool pool;
    return pool;
  }
};

// ============================================================================
// 内联分配/释放函数 —— 直接使用 thread_local 裸变量做缓存
// ============================================================================
inline thread_local PoolFreeBlock *tl_pool_head_ = nullptr;
inline thread_local std::size_t tl_pool_count_ = 0;

inline void *pool_allocate(std::size_t size) {
  if (size > CoroutineMemoryPool::block_size()) [[unlikely]]
    return ::operator new(size);

  if (tl_pool_head_) [[likely]] {
    PoolFreeBlock *b = tl_pool_head_;
    tl_pool_head_ = b->next;
    tl_pool_count_--;
    return b;
  }

  // 慢速路径：批量补货
  auto &pool = CoroutineMemoryPool::get_instance();
  tl_pool_head_ = pool.refill(CoroutineMemoryPool::refill_batch());
  tl_pool_count_ = CoroutineMemoryPool::refill_batch();

  PoolFreeBlock *b = tl_pool_head_;
  tl_pool_head_ = b->next;
  tl_pool_count_--;
  return b;
}

inline void pool_deallocate(void *ptr, std::size_t size) {
  if (size > CoroutineMemoryPool::block_size()) [[unlikely]] {
    ::operator delete(ptr);
    return;
  }

  auto *b = static_cast<PoolFreeBlock *>(ptr);
  b->next = tl_pool_head_;
  tl_pool_head_ = b;
  tl_pool_count_++;

  // 积攒过多时归还一半
  constexpr std::size_t threshold = CoroutineMemoryPool::refill_batch() * 4;
  if (tl_pool_count_ > threshold) [[unlikely]] {
    std::size_t ret = tl_pool_count_ / 2;
    PoolFreeBlock *head = tl_pool_head_;
    PoolFreeBlock *tail = head;
    for (std::size_t i = 1; i < ret; ++i)
      tail = tail->next;
    tl_pool_head_ = tail->next;
    tl_pool_count_ -= ret;
    CoroutineMemoryPool::get_instance().return_blocks(head, tail);
  }
}