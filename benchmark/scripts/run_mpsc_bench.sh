#!/bin/bash
# ============================================================================
# MPSC 无锁队列 Benchmark
# 
# 对比：RingBufferMPSC vs std::mutex+queue vs Boost.Lockfree
# 场景：4 个生产者线程 → 1 个消费者线程
# ============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
RESULT_DIR="$PROJECT_ROOT/bench_docs"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  MPSC Queue Benchmark${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# 检查可执行文件
if [ ! -f "$BUILD_DIR/mpsc_bench" ]; then
    echo -e "${YELLOW}Building mpsc_bench...${NC}"
    cd "$BUILD_DIR" && cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1 && make mpsc_bench -j$(nproc) 2>&1
fi

echo -e "${YELLOW}Running MPSC benchmark (3 repetitions)...${NC}"
echo ""

cd "$BUILD_DIR"
RESULT=$(./mpsc_bench \
    --benchmark_repetitions=3 \
    --benchmark_report_aggregates_only=true \
    --benchmark_format=console 2>&1)

echo "$RESULT"

# 提取关键数据
echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  Summary${NC}"
echo -e "${GREEN}========================================${NC}"

MPSC_TIME=$(echo "$RESULT" | grep "BM_MPSC_Throughput.*_mean" | awk '{print $2}')
MUTEX_TIME=$(echo "$RESULT" | grep "BM_Mutex_Throughput.*_mean" | awk '{print $2}')
BOOST_TIME=$(echo "$RESULT" | grep "BM_BoostLockFree.*_mean" | awk '{print $2}')

echo "  RingBufferMPSC:    ${MPSC_TIME} ns/op"
echo "  std::mutex+queue:  ${MUTEX_TIME} ns/op"
echo "  Boost.Lockfree:    ${BOOST_TIME} ns/op"
echo ""

# 保存结果
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTFILE="$RESULT_DIR/mpsc_bench_${TIMESTAMP}.txt"
echo "$RESULT" > "$OUTFILE"
echo -e "${GREEN}Results saved to: $OUTFILE${NC}"
