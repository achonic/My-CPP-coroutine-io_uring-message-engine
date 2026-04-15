#include <arpa/inet.h>
#include <benchmark/benchmark.h>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

static int target_port = 8888;

static void BM_EchoThroughput(benchmark::State &state) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    state.SkipWithError("socket creation failed");
    return;
  }

  int flag = 1;
  setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

  struct sockaddr_in serv_addr;
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(target_port);

  if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
    state.SkipWithError("Invalid address");
    close(sockfd);
    return;
  }

  if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
    state.SkipWithError(strerror(errno));
    close(sockfd);
    return;
  }

  char send_msg[128] = "Hello, high performance async IO!";
  char recv_msg[128];
  for (auto _ : state) {
    int bytes_sent = send(sockfd, send_msg, sizeof(send_msg), 0);
    if (bytes_sent <= 0) {
      state.SkipWithError("Send failed");
      break;
    }

    int bytes_recv = 0;
    while (bytes_recv < bytes_sent) {
      int ret =
          recv(sockfd, recv_msg + bytes_recv, sizeof(recv_msg) - bytes_recv, 0);
      if (ret <= 0) {
        state.SkipWithError("Recv failed");
        break;
      }
      bytes_recv += ret;
    }
  }
  state.SetBytesProcessed(state.iterations() * sizeof(send_msg) * 2);

  close(sockfd);
}

BENCHMARK(BM_EchoThroughput)->ThreadRange(1, 128)->UseRealTime();

int main(int argc, char **argv) {
  if (const char *env_p = std::getenv("PORT")) {
    target_port = std::stoi(env_p);
  }

  ::benchmark::Initialize(&argc, argv);
  if (::benchmark::ReportUnrecognizedArguments(argc, argv))
    return 1;
  ::benchmark::RunSpecifiedBenchmarks();
  ::benchmark::Shutdown();
  return 0;
}
