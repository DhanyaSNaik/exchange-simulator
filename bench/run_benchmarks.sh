#!/usr/bin/env bash
# Runs the benchmark binary for each (scale, engine) combination as a
# SEPARATE process, so each reported peak-RSS figure is attributable to
# that one engine at that one scale rather than a running high-water mark
# shared across multiple engines in one process.
set -euo pipefail

BIN="${1:-./run_benchmark}"
SEED="${2:-42}"

echo "=== Benchmark sweep: seed=${SEED} ==="
for scale in 100000 1000000 10000000; do
    echo "--- N=${scale} ---"
    "$BIN" "$scale" "$SEED" map
    "$BIN" "$scale" "$SEED" flat
done
