#pragma once

#include "io_awaiter.hpp"
#include "task.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

// 类内 static 以及函数内 static
// 才能够进程/线程共享变量，在内存的数据段/线程数据段里

#include "runtime.hpp"

namespace Engine {

class Socket {
protected:
  int fd_{-1};

public:
  // = default 可以让构造函数可能是 trivial 的，让编译器能优化
  Socket() = default;
  explicit Socket(int fd) : fd_(fd) {}

  ~Socket() {
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }

  // 禁用拷贝
  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;

  // 支持移动
  Socket(Socket &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

  Socket &operator=(Socket &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0)
        close(fd_);
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  int fd() const { return fd_; }
};

class TcpStream : public Socket {
public:
  TcpStream() = default;
  explicit TcpStream(int fd) : Socket(fd) {}

  TcpStream(TcpStream &&) = default;
  TcpStream &operator=(TcpStream &&) = default;

  auto read(void *buf, size_t len) {
    return AsyncRecv(Runtime::get_io_context(), fd_, buf, len);
  }

  auto write(const void *buf, size_t len) {
    return AsyncSend(Runtime::get_io_context(), fd_, buf, len);
  }
};

class TcpListener : public Socket {
public:
  TcpListener(const char *ip, int port) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
      throw std::runtime_error("Failed to create socket");
    }

    int opt = 1;
    if (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
      close(fd_);
      throw std::runtime_error("Failed to set SO_REUSEADDR");
    }

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    // 指定具体 ip 地址
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
      close(fd_);
      throw std::runtime_error("Invalid IP address");
    }

    if (bind(fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
      close(fd_);
      throw std::runtime_error("Failed to bind socket");
    }

    if (listen(fd_, 128) < 0) {
      close(fd_);
      throw std::runtime_error("Failed to listen on socket");
    }
  }

  TcpListener(TcpListener &&) = default;
  TcpListener &operator=(TcpListener &&) = default;

  // accept 返回一个 Task<TcpStream>
  Task<TcpStream> accept() {
    struct sockaddr_in client_addr {};
    socklen_t client_len = sizeof(client_addr);

    // 使用协程要注意，co_await 返回的不是后面跟着的函数返回的类型
    // 返回的是在 awaiter_resume 里定义的返回值
    // co_await
    // 调用的对象就是后面跟着的函数返回的对象，然后编译器去这个对象里面找
    // Awaiter 三部曲。（ 这个对象本身是 awaiter ，或者对象里有operator
    // co_await() ）
    int client_fd =
        co_await AsyncAccept(Runtime::get_io_context(), fd_,
                             (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
      // 发生错误时，返回一个 fd 为 -1 的 TcpStream
      co_return TcpStream{-1};
    }
    co_return TcpStream{client_fd};
  }
};

} // namespace Engine
