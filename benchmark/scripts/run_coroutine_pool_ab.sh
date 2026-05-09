#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
RESULT_DIR="$PROJECT_ROOT/bench_docs"
REPETITIONS=${1:-5}

run_variant() {
  local variant_name="$1"
  local pool_flag="$2"

  echo "========================================"
  echo "  Coroutine Pool A/B: $variant_name"
  echo "========================================"

  cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DMYIO_ENABLE_CORO_POOL="$pool_flag" > /dev/null
  cmake --build "$BUILD_DIR" --target coroutine_bench -j4 > /dev/null

  local result
  result=$("$BUILD_DIR/coroutine_bench" \
    --benchmark_repetitions="$REPETITIONS" \
    --benchmark_report_aggregates_only=true \
    --benchmark_format=console 2>&1)

  echo "$result"
  echo

  local outfile="$RESULT_DIR/coroutine_pool_${variant_name}_$(date +%Y%m%d_%H%M%S).txt"
  echo "$result" > "$outfile"
  echo "Saved to: $outfile"
  echo
}

run_variant "pool_on" ON
run_variant "pool_off" OFF
