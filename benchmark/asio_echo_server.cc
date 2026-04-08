#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <iostream>

using boost::asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::use_awaitable;

awaitable<void> handle_client(tcp::socket socket) {
    try {
        char data[1024];
        while (true) {
            std::size_t n = co_await socket.async_read_some(boost::asio::buffer(data), use_awaitable);
            co_await boost::asio::async_write(socket, boost::asio::buffer(data, n), use_awaitable);
        }
    } catch (std::exception& e) {
        // Ignored for benchmark cleanlyness
        // std::cout << "[AsioServer] Client exception: " << e.what() << std::endl;
    }
}

awaitable<void> listener(tcp::acceptor acceptor) {
    std::cout << "[AsioServer] Listening for connections..." << std::endl;
    try {
        while (true) {
            tcp::socket socket = co_await acceptor.async_accept(use_awaitable);
            // std::cout << "[AsioServer] New connection accepted." << std::endl;
            co_spawn(acceptor.get_executor(), handle_client(std::move(socket)), detached);
        }
    } catch (std::exception& e) {
        std::cout << "[AsioServer] Listener exception: " << e.what() << std::endl;
    }
}

int main(int argc, char* argv[]) {
    try {
        int port = 8889;
        if (argc > 1) port = std::stoi(argv[1]);

        // Single-threaded core (run to match our CoroutineScheduler's logic of 1 Worker CPU)
        boost::asio::io_context io_context(1); 

        tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), port));

        co_spawn(io_context, listener(std::move(acceptor)), detached);

        std::cout << "[AsioServer] Echo Server started on port " << port << " (Using Boost.Asio + Epoll)" << std::endl;

        io_context.run();
    } catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    return 0;
}
