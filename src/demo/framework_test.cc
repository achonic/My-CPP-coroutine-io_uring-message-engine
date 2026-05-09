#include "../include/net.hpp"
#include "../include/runtime.hpp"
#include "../include/scheduler.hpp"
#include <iostream>
#include <thread>
#include <chrono>

using namespace Engine;

// 业务协程：处理单个客户端连接
Task<int> handle_client(TcpStream stream) {
    char buffer[1024];
    std::cout << "[Server] New client session started, fd: " << stream.fd() << std::endl;
    
    try {
        while (true) {
            // 极简 API：隐式获取 IoContext，直接 co_await
            int bytes_recv = co_await stream.read(buffer, sizeof(buffer));
            if (bytes_recv <= 0) {
                std::cout << "[Server] Client disconnected, fd: " << stream.fd() << std::endl;
                break; 
            }
            
            buffer[bytes_recv] = '\0';
            std::cout << "[Server] Received: " << buffer;

            // 极简 API：直接回写
            co_await stream.write(buffer, bytes_recv);
        }
    } catch (const std::exception& e) {
        std::cerr << "[Server] Client error: " << e.what() << std::endl;
    }
    
    co_return 0;
}

// 主协程：监听端口并接受连接
Task<int> server_main(int port) {
    TcpListener listener("0.0.0.0", port);
    std::cout << "[Server] Echo Server started on port " << port << std::endl;

    while (true) {
        // 极简 API：直接 co_await 接受新连接
        TcpStream stream = co_await listener.accept();
        if (stream.fd() < 0) {
            std::cerr << "[Server] Accept failed" << std::endl;
            continue;
        }
        
        // 极简 API：使用 Runtime::spawn 投递新任务，无需再传递 scheduler 指针
        Runtime::spawn(handle_client(std::move(stream)));
    }
    co_return 0;
}

int main() {
    std::cout << "[Main] Configuring Runtime Engine..." << std::endl;
    std::cout << "[Main] Press Ctrl+C to stop the server gracefully." << std::endl;
    
    // 使用全新的 RuntimeBuilder 启动
    RuntimeBuilder builder;
    builder.set_queue_capacity(65536)
           .set_io_uring_entries(4096)
           .set_bind_cpu(-1);

    // block_on 会阻塞主线程，接管信号处理，并在内部跑起 MPSC 调度器
    return builder.block_on(server_main(8889));
}
