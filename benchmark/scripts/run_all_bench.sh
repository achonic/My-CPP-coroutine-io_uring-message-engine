#!/bin/bash
# ============================================================================
# 全量 Benchmark 一键执行
# 
# 依次执行：MPSC → 协程 → IO Echo 对比
# 所有结果保存到 bench_docs/ 目录
#
# 用法：
#   ./run_all_bench.sh             # 执行所有测试
#   ./run_all_bench.sh --io-only   # 只跑 IO 对比测试
# ============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}╔══════════════════════════════════════╗${NC}"
echo -e "${GREEN}║  Full Benchmark Suite                ║${NC}"
echo -e "${GREEN}║  MyIOMessageEngine                   ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════╝${NC}"
echo ""

# 确保 build 目录存在
if [ ! -d "$BUILD_DIR" ]; then
    mkdir -p "$BUILD_DIR"
fi

# 全量构建
echo -e "${YELLOW}[1/4] Building all targets...${NC}"
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
make -j$(nproc) 2>&1 | tail -5
echo ""

if [ "$1" = "--io-only" ]; then
    echo -e "${YELLOW}[--io-only] Skipping MPSC and Coroutine benchmarks${NC}"
    echo ""
    echo -e "${YELLOW}[4/4] IO Echo Benchmark...${NC}"
    echo ""
    bash "$SCRIPT_DIR/run_io_bench.sh" 3
    exit 0
fi

# MPSC
echo -e "${YELLOW}[2/4] MPSC Queue Benchmark...${NC}"
echo ""
bash "$SCRIPT_DIR/run_mpsc_bench.sh"
echo ""
echo ""

# Coroutine
echo -e "${YELLOW}[3/4] Coroutine Overhead Benchmark...${NC}"
echo ""
bash "$SCRIPT_DIR/run_coroutine_bench.sh"
echo ""
echo ""

# IO Echo
echo -e "${YELLOW}[4/4] IO Echo Benchmark...${NC}"
echo ""
bash "$SCRIPT_DIR/run_io_bench.sh" 3
echo ""

echo -e "${GREEN}╔══════════════════════════════════════╗${NC}"
echo -e "${GREEN}║  All benchmarks completed!           ║${NC}"
echo -e "${GREEN}║  Results in: bench_docs/             ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════╝${NC}"
