#!/bin/bash
# ============================================================================
# IO Echo 吞吐对比 Benchmark
# 
# 对比：io_uring (framework_test) vs Boost.Asio (asio_echo_server)
# 场景：Echo Server，客户端 1→128 并发线程
#
# 用法：
#   ./run_io_bench.sh              # 默认：2次重复
#   ./run_io_bench.sh 5            # 5次重复（更精确）
# ============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
RESULT_DIR="$PROJECT_ROOT/bench_docs"

REPETITIONS=${1:-2}

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

URING_PORT=8889
ASIO_PORT=8888

cleanup() {
    echo -e "\n${YELLOW}Cleaning up servers...${NC}"
    pkill -9 -f "framework_test" 2>/dev/null || true
    pkill -9 -f "asio_echo_server" 2>/dev/null || true
    sleep 1
}

trap cleanup EXIT

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  IO Echo Throughput Benchmark${NC}"
echo -e "${GREEN}  io_uring vs Boost.Asio (epoll)${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo "  Repetitions: $REPETITIONS"
echo "  io_uring port: $URING_PORT"
echo "  Asio port:     $ASIO_PORT"
echo ""

# 构建
if [ ! -f "$BUILD_DIR/framework_test" ] || [ ! -f "$BUILD_DIR/asio_echo_server" ] || [ ! -f "$BUILD_DIR/echo_client_bench" ]; then
    echo -e "${YELLOW}Building targets...${NC}"
    cd "$BUILD_DIR" && cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1 && make framework_test asio_echo_server echo_client_bench -j$(nproc) 2>&1
fi

# 确保端口可用
cleanup 2>/dev/null

# ─── 启动 io_uring Echo Server ───
echo -e "${CYAN}Starting io_uring Echo Server (port $URING_PORT)...${NC}"
cd "$BUILD_DIR"
./framework_test > /dev/null 2>&1 &
URING_PID=$!
sleep 2

if ! kill -0 $URING_PID 2>/dev/null; then
    echo -e "${RED}ERROR: io_uring server failed to start${NC}"
    exit 1
fi
echo -e "  PID: $URING_PID ✓"

# ─── 启动 Boost.Asio Echo Server ───
echo -e "${CYAN}Starting Boost.Asio Echo Server (port $ASIO_PORT)...${NC}"
./asio_echo_server $ASIO_PORT > /dev/null 2>&1 &
ASIO_PID=$!
sleep 2

if ! kill -0 $ASIO_PID 2>/dev/null; then
    echo -e "${RED}ERROR: Asio server failed to start${NC}"
    exit 1
fi
echo -e "  PID: $ASIO_PID ✓"

# 确认后端
echo ""
echo -e "${YELLOW}Verifying backends...${NC}"
ss -tlnp | grep -E "$URING_PORT|$ASIO_PORT" 2>/dev/null
echo ""

# ─── 测试 io_uring ───
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  Testing io_uring Server${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

URING_RESULT=$(PORT=$URING_PORT ./echo_client_bench \
    --benchmark_repetitions=$REPETITIONS \
    --benchmark_report_aggregates_only=true \
    --benchmark_format=console 2>&1 | grep -E "^(BM_|Running|Run on|CPU|Load|---|\*\*\*)")

echo "$URING_RESULT"

# ─── 测试 Boost.Asio ───
echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  Testing Boost.Asio (epoll) Server${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

ASIO_RESULT=$(PORT=$ASIO_PORT ./echo_client_bench \
    --benchmark_repetitions=$REPETITIONS \
    --benchmark_report_aggregates_only=true \
    --benchmark_format=console 2>&1 | grep -E "^(BM_|Running|Run on|CPU|Load|---|\*\*\*)")

echo "$ASIO_RESULT"

# ─── 对比摘要 ───
echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  Side-by-Side Comparison (median)${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
printf "  %-12s  %16s  %16s  %s\n" "Threads" "io_uring" "Asio(epoll)" "Winner"
printf "  %-12s  %16s  %16s  %s\n" "───────" "────────────────" "────────────────" "──────"

for T in 1 2 4 8 16 32 64 128; do
    URING_BPS=$(echo "$URING_RESULT" | grep "threads:${T}_median" | grep -oP '[\d.]+Mi/s' | head -1)
    ASIO_BPS=$(echo "$ASIO_RESULT" | grep "threads:${T}_median" | grep -oP '[\d.]+Mi/s' | head -1)
    
    if [ -n "$URING_BPS" ] && [ -n "$ASIO_BPS" ]; then
        URING_VAL=$(echo "$URING_BPS" | grep -oP '[\d.]+')
        ASIO_VAL=$(echo "$ASIO_BPS" | grep -oP '[\d.]+')
        
        if [ -n "$URING_VAL" ] && [ -n "$ASIO_VAL" ]; then
            WINNER=$(echo "$URING_VAL $ASIO_VAL" | awk '{
                if ($1 > $2 * 1.05) print "io_uring ✓"
                else if ($2 > $1 * 1.05) print "Asio ✓"
                else print "≈ tie"
            }')
        else
            WINNER="?"
        fi
    else
        URING_BPS="${URING_BPS:-ERROR}"
        ASIO_BPS="${ASIO_BPS:-ERROR}"
        WINNER="?"
    fi
    
    printf "  %-12s  %16s  %16s  %s\n" "$T" "$URING_BPS" "$ASIO_BPS" "$WINNER"
done

echo ""

# 保存结果
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
{
    echo "# IO Echo Benchmark Results - $TIMESTAMP"
    echo ""
    echo "## io_uring Server (port $URING_PORT)"
    echo "$URING_RESULT"
    echo ""
    echo "## Boost.Asio Server (port $ASIO_PORT)"
    echo "$ASIO_RESULT"
} > "$RESULT_DIR/io_bench_${TIMESTAMP}.txt"

echo -e "${GREEN}Results saved to: $RESULT_DIR/io_bench_${TIMESTAMP}.txt${NC}"
