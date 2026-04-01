#include "io_context.hpp"
#include <iostream>
#include <cassert>

int main() {
    try {
        IoContext io(64);
        std::cout << "io_uring initialized successfully." << std::endl;
        
        struct io_uring_sqe* sqe = io.get_sqe();
        assert(sqe != nullptr);
        std::cout << "Successfully got an SQE." << std::endl;
        
        int submitted = io.submit();
        std::cout << "Submitted " << submitted << " entries." << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
