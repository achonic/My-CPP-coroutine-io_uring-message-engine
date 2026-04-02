#include "../include/scheduler.hpp"
#include "../include/io_awaiter.hpp"
#include "../include/task.hpp"
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <signal.h>

/**
 * @brief 处理单个客户端连接的协程
 * 每个连接拥有独立的协程栈帧，逻辑清晰如同步代码
 */
Task<int> handle_client(IoContext &ctx, int client_fd) {
    char buffer[1024];
    while (true) {
        // 1. 异步读取数据
        int bytes_recv = co_await AsyncRecv(ctx, client_fd, buffer, sizeof(buffer));
        if (bytes_recv <= 0) break; // 连接关闭或出错

        // 2. 异步原路发送回写 (Echo)
        int bytes_sent = co_await AsyncSend(ctx, client_fd, buffer, bytes_recv);
        if (bytes_sent <= 0) break;
    }
    
    close(client_fd);
    std::cout << "[Server] Client disconnected, fd: " << client_fd << std::endl;
    co_return 0;
}

/**
 * @brief 监听并接受连接的协程 (Master Coroutine)
 */
Task<int> server_loop(CoroutineScheduler &sched, int listen_fd) {
    IoContext &ctx = sched.get_io_context();
    std::cout << "[Server] Listening for connections..." << std::endl;

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        // 异步 Accept
        int client_fd = co_await AsyncAccept(ctx, listen_fd, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_fd >= 0) {
            std::cout << "[Server] New connection accepted, fd: " << client_fd << std::endl;
            // 收到新连接，立即创建一个新协程并投递给调度器
            // 调度器会在同一个单线程 Worker 中并发处理这些协程
            sched.submit(handle_client(ctx, client_fd));
        }
    }
    co_return 0;
}

int main(int argc, char* argv[]) {
    int port = 8888;
    if (argc > 1) port = std::stoi(argv[1]);

    // 1. 创建监听 Socket (标准流程)
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 128);

    // 2. 初始化调度器
    // 单线程驱动，绑核 CPU 0，io_uring 队列深度 4096
    CoroutineScheduler sched(65536, 0, 4096);
    sched.start();

    std::cout << "[Main] Echo Server started on port " << port << " (Using io_uring + Coroutines)" << std::endl;

    // 3. 提交主监听协程
    sched.submit(server_loop(sched, listen_fd));

    // 防止主线程退出
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    sched.stop();
    close(listen_fd);
    return 0;
}
