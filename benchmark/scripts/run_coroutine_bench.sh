#!/bin/bash
# ============================================================================
# 协程开销 Benchmark
# 
# 测试：创建/销毁、完整生命周期、对称传输 100 层嵌套
# ============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
RESULT_DIR="$PROJECT_ROOT/bench_docs"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  Coroutine Overhead Benchmark${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

if [ ! -f "$BUILD_DIR/coroutine_bench" ]; then
    echo -e "${YELLOW}Building coroutine_bench...${NC}"
    cd "$BUILD_DIR" && cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1 && make coroutine_bench -j$(nproc) 2>&1
fi

echo -e "${YELLOW}Running coroutine benchmark (3 repetitions)...${NC}"
echo ""

cd "$BUILD_DIR"
RESULT=$(./coroutine_bench \
    --benchmark_repetitions=3 \
    --benchmark_report_aggregates_only=true \
    --benchmark_format=console 2>&1)

echo "$RESULT"

echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  Summary${NC}"
echo -e "${GREEN}========================================${NC}"

CREATE_TIME=$(echo "$RESULT" | grep "CreateAndDestroy_mean" | awk '{print $2}')
LIFECYCLE_TIME=$(echo "$RESULT" | grep "FullLifecycle_mean" | awk '{print $2}')
TRANSFER_TIME=$(echo "$RESULT" | grep "SymmetricTransfer.*_mean" | awk '{print $2}')

echo "  Create+Destroy:            ${CREATE_TIME} ns"
echo "  Full Lifecycle:             ${LIFECYCLE_TIME} ns"
echo "  Symmetric Transfer (100层): ${TRANSFER_TIME} ns"

if [ -n "$TRANSFER_TIME" ]; then
    PER_LEVEL=$(echo "$TRANSFER_TIME" | awk '{printf "%.1f", $1/100}')
    echo "  Per-level transfer:         ${PER_LEVEL} ns/层"
fi
echo ""

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTFILE="$RESULT_DIR/coroutine_bench_${TIMESTAMP}.txt"
echo "$RESULT" > "$OUTFILE"
echo -e "${GREEN}Results saved to: $OUTFILE${NC}"
