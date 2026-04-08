#include <benchmark/benchmark.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>

static int target_port = 8888; 

class EchoFixture : public benchmark::Fixture {
public:
    int sockfd = -1;

    void SetUp(::benchmark::State& state) {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            state.SkipWithError("socket creation failed");
            return;
        }

        // Disable Nagle to measure exactly each round-trip immediately
        int flag = 1;
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));

        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(target_port);

        if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
            state.SkipWithError("Invalid address");
            return;
        }

        if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            state.SkipWithError("Connection Failed");
            return;
        }
    }

    void TearDown(::benchmark::State& state) {
        if (sockfd >= 0) {
            close(sockfd);
        }
    }
};

BENCHMARK_DEFINE_F(EchoFixture, BM_EchoThroughput)(benchmark::State& state) {
    char send_msg[128] = "Hello, high performance async IO!";
    char recv_msg[128];
    for (auto _ : state) {
        int bytes_sent = send(sockfd, send_msg, sizeof(send_msg), 0);
        if (bytes_sent <= 0) {
            state.SkipWithError("Send failed");
            break;
        }

        int bytes_recv = 0;
        while(bytes_recv < bytes_sent) {
            int ret = recv(sockfd, recv_msg + bytes_recv, sizeof(recv_msg) - bytes_recv, 0);
            if (ret <= 0) {
                state.SkipWithError("Recv failed");
                break;
            }
            bytes_recv += ret;
        }
    }
    // OPS (Op/s) measurement based on total bytes
    state.SetBytesProcessed(state.iterations() * sizeof(send_msg) * 2);
}

// Measure connections up to 128 concurrently
BENCHMARK_REGISTER_F(EchoFixture, BM_EchoThroughput)
    ->ThreadRange(1, 128)
    ->UseRealTime();

int main(int argc, char** argv) {
    if (const char* env_p = std::getenv("PORT")) {
        target_port = std::stoi(env_p);
    }

    ::benchmark::Initialize(&argc, argv);
    if (::benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    ::benchmark::RunSpecifiedBenchmarks();
    ::benchmark::Shutdown();
    return 0;
}
