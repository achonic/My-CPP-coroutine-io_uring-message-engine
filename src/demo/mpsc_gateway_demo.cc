#include "../include/net.hpp"
#include "../include/runtime.hpp"
#include "../include/scheduler.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

using namespace Engine;

// ============================================================================
//  Multi-Thread Business Gateway Demo
//  
//  核心目标：展示 MPSC 无锁队列的真正价值
//  —— 多个业务线程并发跨线程投递 IO 协程任务到唯一的 IO Worker
//
//  架构：
//    Acceptor (单独线程)       → 同步 accept 新连接，round-robin 分发 fd
//    Business Thread 0..N-1   → 同步 recv + CPU 密集计算 → sched.submit() 跨线程投递
//    IO Worker (单线程)       → MPSC pop → io_uring 异步发送响应
//
//  MPSC 队列的角色：
//    4 个业务线程 同时 push → 唯一的 Worker 线程 pop
//    这是真正的 Multi-Producer, Single-Consumer 场景！
//
//  测试方法：
//    启动后用多个客户端并发发送请求：
//    for i in $(seq 1 100); do echo "request-$i" | nc -q0 localhost 9000 & done
// ============================================================================

constexpr int NUM_BUSINESS_THREADS = 4;
constexpr int GATEWAY_PORT = 9000;

// --- 线程安全的 FD 分发队列（给业务线程用的简单阻塞队列）---
class FdDispatchQueue {
private:
    std::queue<int> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool shutdown_ = false;

public:
    void push(int fd) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            queue_.push(fd);
        }
        cv_.notify_one();
    }

    // 返回 -1 表示 shutdown
    int pop() {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return !queue_.empty() || shutdown_; });
        if (shutdown_ && queue_.empty()) return -1;
        int fd = queue_.front();
        queue_.pop();
        return fd;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }
};

// 全局统计
std::atomic<int> g_total_requests{0};
std::atomic<int> g_total_replies_sent{0};

// 业务线程的 fd 分发队列（Round-Robin）
FdDispatchQueue g_dispatch_queues[NUM_BUSINESS_THREADS];

// ============================================================================
//  IO 协程：通过 io_uring 异步发送响应（运行在 Worker 线程上）
//  
//  这个协程是被业务线程通过 sched.submit() 跨线程投递到 MPSC 队列的。
//  Worker 线程从 MPSC pop 出来后 resume 它，此时才真正执行 io_uring 异步发送。
// ============================================================================
Task<int> async_reply_task(int client_fd, std::string response) {
    // 此时我们已经在 Worker 线程上了，可以安全使用 io_uring
    IoContext& ctx = Runtime::get_io_context();
    
    int bytes_sent = co_await AsyncSend(ctx, client_fd, 
                                         response.c_str(), response.size());
    
    if (bytes_sent > 0) {
        g_total_replies_sent.fetch_add(1, std::memory_order_relaxed);
    }

    // 发送完成后关闭连接（短连接模式）
    close(client_fd);

    co_return bytes_sent;
}

// ============================================================================
//  模拟 CPU 密集的业务处理
//  真实场景中可能是：JSON 解析、JWT 校验、签名验证、protobuf 序列化等
// ============================================================================
std::string do_heavy_business_processing(const char* request, int len, int thread_id) {
    // 模拟 CPU 密集计算
    volatile int checksum = 0;
    for (int i = 0; i < len; ++i) {
        checksum += request[i];
    }
    // 模拟更重的计算（如加密/签名验证）
    for (int i = 0; i < 5000; ++i) {
        checksum ^= (checksum << 3) + i;
    }

    // 构造 HTTP 响应
    std::string body = "Processed by Business-Thread-" + std::to_string(thread_id) 
                     + " | Checksum: " + std::to_string((int)checksum) 
                     + " | Echo: " + std::string(request, len);

    std::string response;
    response.reserve(512);
    response += "HTTP/1.1 200 OK\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "X-Business-Thread: " + std::to_string(thread_id) + "\r\n";
    response += "Connection: close\r\n";
    response += "\r\n";
    response += body;
    
    return response;
}

// ============================================================================
//  业务处理线程：同步 recv + CPU 计算 + 跨线程 submit IO 协程
//  
//  每个业务线程就是 MPSC 的一个 Producer。
//  4 个线程同时调用 sched.submit()，即同时执行 queue_.push()，
//  此时 MPSC 的 CAS 原子操作（head_.fetch_add）
//  才真正被多核并发施压！
//
//  注意：这里直接使用 CoroutineScheduler::submit() 而不是 Runtime::spawn()，
//  因为 Runtime::spawn() 依赖 thread_local 指针，只有在 Worker 线程或
//  block_on 所在的主线程中才可用。而业务线程是独立的外部线程，
//  需要通过 CoroutineScheduler 的引用直接投递任务。
//  这恰恰体现了 MPSC 的设计本质：任何线程都可以是 Producer！
// ============================================================================
void business_thread_func(int thread_id, CoroutineScheduler& sched) {
    std::cout << "[Biz-" << thread_id << "] Started. Waiting for connections..." << std::endl;
    
    char buffer[4096];
    
    while (true) {
        // 1. 从分发队列阻塞等待新的客户端 fd
        int client_fd = g_dispatch_queues[thread_id].pop();
        if (client_fd < 0) {
            // shutdown 信号
            break;
        }

        // 2. 同步 recv —— 业务线程直接读取客户端数据
        //    不走 io_uring（业务线程没有 IoContext，也不需要）
        int bytes_recv = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes_recv <= 0) {
            close(client_fd);
            continue;
        }
        buffer[bytes_recv] = '\0';
        
        g_total_requests.fetch_add(1, std::memory_order_relaxed);

        // 3. CPU 密集的业务处理（耗时操作，不阻塞 IO Worker）
        std::string response = do_heavy_business_processing(
            buffer, bytes_recv, thread_id);

        // 4. 【核心！MPSC 的真正战场】
        //    跨线程投递 IO 协程到 Worker 的 MPSC 队列。
        //    4 个业务线程在这里并发调用 sched.submit()，
        //    同时 push 到同一个 RingBufferMPSC，触发真正的 CAS 竞争。
        //    投递完成后业务线程立即释放，回去等下一个 fd。
        sched.submit(async_reply_task(client_fd, std::move(response)));
    }
    
    std::cout << "[Biz-" << thread_id << "] Shutdown." << std::endl;
}

// ============================================================================
//  Acceptor 线程：同步 accept 新连接，Round-Robin 分发给业务线程
//
//  为什么不用协程 accept？因为我们要展示外部线程投递的场景。
//  如果 accept 也在 Worker 上，那 spawn 又变成单线程自产自消了。
// ============================================================================
void acceptor_thread_func(int port) {
    // 创建监听 socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "[Acceptor] Failed to create socket" << std::endl;
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[Acceptor] Bind failed" << std::endl;
        close(listen_fd);
        return;
    }

    if (listen(listen_fd, 128) < 0) {
        std::cerr << "[Acceptor] Listen failed" << std::endl;
        close(listen_fd);
        return;
    }

    std::cout << "[Acceptor] Listening on port " << port << std::endl;

    int rr_index = 0;

    // 使用 poll 超时来避免 accept 永久阻塞，使线程可以响应 shutdown 信号
    struct pollfd pfd;
    pfd.fd = listen_fd;
    pfd.events = POLLIN;

    while (!Engine::g_stop_flag.load(std::memory_order_acquire)) {
        // 500ms 超时，定期检查 stop flag
        int poll_ret = poll(&pfd, 1, 500);
        if (poll_ret <= 0) continue;

        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            continue;
        }

        // Round-Robin 分发到业务线程
        int target = rr_index % NUM_BUSINESS_THREADS;
        g_dispatch_queues[target].push(client_fd);
        rr_index++;
    }

    close(listen_fd);
    std::cout << "[Acceptor] Shutdown." << std::endl;
}

// ============================================================================
//  空的 Gateway 主协程
//  仅作为 block_on 的入口，确保 Runtime 环境被初始化。
//  真正的 accept 和业务处理由独立的外部线程完成。
// ============================================================================
Task<int> gateway_idle() {
    // 这个协程挂起后，Worker 线程就开始专注于消费 MPSC 队列 + 收割 io_uring CQE
    // 不需要做任何事，业务线程会通过 sched.submit() 不断投递任务
    co_return 0;
}

// ============================================================================
//  监控线程：定期打印 MPSC 吞吐统计
// ============================================================================
void monitor_thread_func() {
    int last_requests = 0;
    int last_replies = 0;
    
    while (!Engine::g_stop_flag.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        int cur_requests = g_total_requests.load(std::memory_order_relaxed);
        int cur_replies = g_total_replies_sent.load(std::memory_order_relaxed);
        
        int delta_req = cur_requests - last_requests;
        int delta_rep = cur_replies - last_replies;
        
        if (cur_requests > 0 || cur_replies > 0) {
            std::cout << "\n[Monitor] Requests: " << cur_requests
                      << " | Replies(io_uring): " << cur_replies
                      << " | Δreq/2s: " << delta_req
                      << " | Δrep/2s: " << delta_rep
                      << " | MPSC→Worker pending: " << (cur_requests - cur_replies)
                      << std::endl;
        }
        
        last_requests = cur_requests;
        last_replies = cur_replies;
    }
}

// ============================================================================
//  Main
//
//  注意启动顺序：
//  1. 先手动创建 CoroutineScheduler（不用 block_on 自动创建）
//  2. 把 sched 引用传给业务线程（它们是 MPSC 的 Producer）
//  3. 启动 Worker 线程（它是 MPSC 唯一的 Consumer）
//  4. 主线程轮询 stop flag
//
//  这样做的原因：block_on 会在栈上创建 CoroutineScheduler，
//  但业务线程需要在 block_on 之前/同时启动，并且需要 sched 的引用。
//  所以我们手动搭建 Runtime 环境，这也更清楚地展示了框架各组件的协作。
// ============================================================================
int main() {
    std::cout << "==========================================================\n";
    std::cout << "  MPSC Lock-Free Queue Demo: Multi-Thread Business Gateway\n";
    std::cout << "==========================================================\n";
    std::cout << "\n";
    std::cout << "  Architecture:\n";
    std::cout << "    [Acceptor Thread] → accept → Round-Robin → [Biz Threads]\n";
    std::cout << "    [Biz Thread 0..3] → recv + CPU work → sched.submit()\n";
    std::cout << "                               ↓ (MPSC lock-free push × 4 threads)\n";
    std::cout << "    [IO Worker]       → MPSC pop → io_uring async send\n";
    std::cout << "\n";
    std::cout << "  MPSC Producers: " << NUM_BUSINESS_THREADS << " business threads\n";
    std::cout << "  MPSC Consumer:  1 IO Worker thread\n";
    std::cout << "\n";
    std::cout << "  Test commands:\n";
    std::cout << "    echo 'Hello' | nc -q0 localhost " << GATEWAY_PORT << "\n";
    std::cout << "    for i in $(seq 1 200); do echo \"req-$i\" | nc -q0 localhost "
              << GATEWAY_PORT << " & done\n";
    std::cout << "\n";
    std::cout << "  Press Ctrl+C to stop.\n";
    std::cout << "==========================================================\n\n";

    // 1. 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 2. 手动创建调度器（等价于 block_on 内部做的事情）
    CoroutineScheduler sched(65536, -1, 4096);
    
    // 3. 为主线程绑定 Runtime 环境
    Runtime::set_current_scheduler(&sched);

    // 4. 投递空的入口任务，让 Worker 有事可做（立即完成）
    sched.submit(gateway_idle());

    // 5. 启动 Worker 线程（MPSC 的唯一 Consumer）
    sched.start([&sched]() {
        Runtime::set_current_scheduler(&sched);
    });

    // 6. 启动业务线程（MPSC 的 4 个 Producer）
    //    它们拿着 sched 的引用，可以随时跨线程 submit 任务
    std::vector<std::thread> biz_threads;
    for (int i = 0; i < NUM_BUSINESS_THREADS; ++i) {
        biz_threads.emplace_back(business_thread_func, i, std::ref(sched));
    }

    // 7. 启动 Acceptor 线程
    std::thread acceptor(acceptor_thread_func, GATEWAY_PORT);

    // 8. 启动监控线程
    std::thread monitor(monitor_thread_func);

    // 9. 主线程轮询等待退出信号（等价于 block_on 里的循环）
    while (!g_stop_flag.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\n[Runtime] Received Ctrl+C. Graceful shutdown..." << std::endl;

    // 10. 优雅关停
    // 先关业务线程，再关调度器
    for (int i = 0; i < NUM_BUSINESS_THREADS; ++i) {
        g_dispatch_queues[i].shutdown();
    }
    for (auto& t : biz_threads) {
        if (t.joinable()) t.join();
    }

    // 等待 Acceptor 线程退出（poll 超时后检查 stop flag）
    if (acceptor.joinable()) acceptor.join();
    
    // 等待监控线程
    if (monitor.joinable()) monitor.join();

    // 最后停调度器
    sched.stop();

    std::cout << "\n==========================================================\n";
    std::cout << "  Final Statistics:\n";
    std::cout << "    Total requests received:     " << g_total_requests.load() << "\n";
    std::cout << "    Total replies via io_uring:   " << g_total_replies_sent.load() << "\n";
    std::cout << "    All IO was funneled through MPSC from " << NUM_BUSINESS_THREADS 
              << " threads\n";
    std::cout << "==========================================================\n";

    return 0;
}
