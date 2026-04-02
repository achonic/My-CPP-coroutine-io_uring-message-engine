#include "../include/scheduler.hpp"
#include "../include/io_awaiter.hpp"
#include "../include/task.hpp"
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cassert>
#include <atomic>

std::atomic<bool> test_finished{false};
std::string final_content;

/**
 * @brief 核心集成协程：演示在调度器中进行连续异步 IO 操作
 */
Task<int> integrated_io_coroutine(CoroutineScheduler &sched, const char *filename) {
    IoContext &ctx = sched.get_io_context();
    const char *data_to_write = "Integrated IO Test with Coroutine Scheduler!";
    size_t len = strlen(data_to_write);

    // 1. 异步写入文件
    int fd_write = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_write < 0) co_return -1;

    std::cout << "[Coroutine] Async Writing to " << filename << "..." << std::endl;
    int written = co_await AsyncWrite(ctx, fd_write, data_to_write, len);
    close(fd_write);

    if (written < 0) {
        std::cerr << "[Coroutine] Write failed: " << written << std::endl;
        co_return -1;
    }
    std::cout << "[Coroutine] Write success, bytes: " << written << std::endl;

    // 2. 异步读取文件
    int fd_read = open(filename, O_RDONLY);
    if (fd_read < 0) co_return -1;

    char read_buf[128] = {0};
    std::cout << "[Coroutine] Async Reading from " << filename << "..." << std::endl;
    int read_bytes = co_await AsyncRead(ctx, fd_read, read_buf, sizeof(read_buf) - 1);
    close(fd_read);

    if (read_bytes >= 0) {
        read_buf[read_bytes] = '\0';
        final_content = read_buf;
        std::cout << "[Coroutine] Read success: " << final_content << std::endl;
    } else {
        std::cerr << "[Coroutine] Read failed: " << read_bytes << std::endl;
    }

    test_finished.store(true, std::memory_order_release);
    co_return read_bytes;
}

int main() {
    const char *filename = "integrated_test.txt";
    
    // 初始化调度器：1024 队列深度，不绑核 (-1)，io_uring 深度 64
    CoroutineScheduler sched(1024, -1, 64);
    sched.start();

    std::cout << "[Main] Submitting integrated IO task..." << std::endl;
    
    // 提交任务到调度器
    sched.submit(integrated_io_coroutine(sched, filename));

    // 等待协程执行完毕（由调度器的 run_loop 驱动 IO 收割和唤醒）
    int timeout_ms = 2000;
    while (!test_finished.load(std::memory_order_acquire) && timeout_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        timeout_ms -= 10;
    }

    if (test_finished.load()) {
        std::cout << "[Main] Test completed successfully!" << std::endl;
        assert(final_content == "Integrated IO Test with Coroutine Scheduler!");
    } else {
        std::cerr << "[Main] Test TIMEOUT! Internal loop might be broken." << std::endl;
    }

    sched.stop();
    unlink(filename);

    return test_finished.load() ? 0 : 1;
}
