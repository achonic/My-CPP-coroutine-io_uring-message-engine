#include <benchmark/benchmark.h>
#include "task.hpp"
#include "memory_pool.hpp"

// 1. 空协程：用于测试协程帧的创建（带内存池分配/释放）和销毁开销。
Task<int> empty_task() {
    co_return 0;
}

// 单纯创建与销毁：测试协程状态机在无阻塞情况下的最小装载开销
static void BM_Coroutine_CreateAndDestroy(benchmark::State& state) {
    reset_pool_stats();
    for (auto _ : state) {
        auto t = empty_task();
        benchmark::DoNotOptimize(t);
    }
    state.counters["pool_allocs"] = static_cast<double>(pool_stats().pool_allocations);
    state.counters["fallback_allocs"] = static_cast<double>(pool_stats().fallback_allocations);
    state.counters["pool_deallocs"] = static_cast<double>(pool_stats().pool_deallocations);
    state.counters["fallback_deallocs"] = static_cast<double>(pool_stats().fallback_deallocations);
}
BENCHMARK(BM_Coroutine_CreateAndDestroy);

// 此时测试 创建、初次挂起、resume运行结束并销毁 的全周期
static void BM_Coroutine_FullLifecycle(benchmark::State& state) {
    reset_pool_stats();
    for (auto _ : state) {
        auto t = empty_task();
        t.resume(); // 一次resume后即执行到co_return，结束状态机
    }
    state.counters["pool_allocs"] = static_cast<double>(pool_stats().pool_allocations);
    state.counters["fallback_allocs"] = static_cast<double>(pool_stats().fallback_allocations);
    state.counters["pool_deallocs"] = static_cast<double>(pool_stats().pool_deallocations);
    state.counters["fallback_deallocs"] = static_cast<double>(pool_stats().fallback_deallocations);
}
BENCHMARK(BM_Coroutine_FullLifecycle);

// 2. 对称传输（Symmetric Transfer）上下文切换开销
// 深度嵌套 co_await 会频繁触发协程 state machine 的 await_suspend 返回 continuation_ 进行硬件跳转
Task<int> dummy_child() { 
    co_return 1; 
}

Task<int> dummy_parent(int depth) {
    if (depth == 0) co_return co_await dummy_child();
    co_return co_await dummy_parent(depth - 1);
}

// 模拟深度递归（100层）情况下的对称传输花销
static void BM_Coroutine_SymmetricTransfer_Depth100(benchmark::State& state) {
    reset_pool_stats();
    for (auto _ : state) {
        auto t = dummy_parent(100);
        t.resume(); // 从最外层开始，向下 await 100 层，然后再 100 层逐级返回
    }
    state.counters["pool_allocs"] = static_cast<double>(pool_stats().pool_allocations);
    state.counters["fallback_allocs"] = static_cast<double>(pool_stats().fallback_allocations);
    state.counters["pool_deallocs"] = static_cast<double>(pool_stats().pool_deallocations);
    state.counters["fallback_deallocs"] = static_cast<double>(pool_stats().fallback_deallocations);
}
BENCHMARK(BM_Coroutine_SymmetricTransfer_Depth100);

int main(int argc, char** argv) {
    reset_pool_stats();
    ::benchmark::Initialize(&argc, argv);
    if (::benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    ::benchmark::RunSpecifiedBenchmarks();
    return 0;
}
