#include "../include/scheduler.hpp"
#include "../include/io_awaiter.hpp"
#include "../include/task.hpp"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <atomic>
#include <chrono>

/**
 * 这是一个“异步审计日志”场景。
 * 多个业务线程产生日志，单个调度器负责通过 io_uring 落盘。
 */

constexpr int NUM_PRODUCERS = 4;
constexpr int LOGS_PER_PRODUCER = 10000; // 每个线程产生 1 万条日志
std::atomic<int> logs_completed{0};

/**
 * 日志落盘协程
 */
Task<int> audit_log_task(IoContext &ctx, int fd, std::string message) {
    // 使用 io_uring 异步写入
    // 注意：在 O_APPEND 模式下，多个 SQE 提交是安全的
    int res = co_await AsyncWrite(ctx, fd, message.c_str(), message.length());
    
    if (res > 0) {
        logs_completed.fetch_add(1, std::memory_order_relaxed);
    }
    co_return res;
}

/**
 * 模拟业务生产线程
 */
void business_thread(CoroutineScheduler &sched, int thread_id, int fd) {
    for (int i = 0; i < LOGS_PER_PRODUCER; ++i) {
        // 模拟业务计算耗时
        if (i % 100 == 0) {
            std::this_thread::yield();
        }

        std::string log_msg = "Thread-" + std::to_string(thread_id) + 
                              " | Log-ID: " + std::to_string(i) + " | Status: OK\n";
        
        // 【核心点】：跨线程投递！通过 MPSC 进入调度器
        sched.submit(audit_log_task(sched.get_io_context(), fd, std::move(log_msg))); 
    }
}

int main() {
    const char* log_file = "stress_audit.log";
    int fd = open(log_file, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (fd < 0) return 1;

    // 1. 初始化调度器 (单线程消费)
    CoroutineScheduler sched(65536, 0, 4096);
    sched.start();

    auto start_time = std::chrono::high_resolution_clock::now();

    // 2. 启动多个生产者线程 (多线程生产)
    std::vector<std::thread> producers;
    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        producers.emplace_back(business_thread, std::ref(sched), i, fd);
    }

    // 3. 等待生产完成
    for (auto &p : producers) {
        p.join();
    }
    std::cout << "[Main] All producers finished submitting." << std::endl;

    // 4. 等待调度器将所有协程执行完毕 (即所有 IO 落盘)
    int total_expected = NUM_PRODUCERS * LOGS_PER_PRODUCER;
    while (logs_completed.load(std::memory_order_relaxed) < total_expected) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;

    std::cout << "========================================" << std::endl;
    std::cout << "MPSC + io_uring Stress Test Results:" << std::endl;
    std::cout << "Total Logs Processed: " << logs_completed.load() << std::endl;
    std::cout << "Time Taken: " << elapsed.count() << " seconds" << std::endl;
    std::cout << "Throughput: " << (logs_completed.load() / elapsed.count()) << " logs/sec" << std::endl;
    std::cout << "========================================" << std::endl;

    sched.stop();
    close(fd);
    // unlink(log_file); // 如果想看文件内容可以不删除
    return 0;
}
