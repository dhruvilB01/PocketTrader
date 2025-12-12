#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

source ./env_pockettrader.sh

echo "[SCENARIO 03] BBB_IP=$BBB_IP (Toxic Latency on EXB)"

cleanup() {
    echo "[SCENARIO 03] Cleaning up..."
    [[ -n "${BRIDGE_PID:-}" ]] && kill "$BRIDGE_PID" 2>/dev/null || true
    [[ -n "${EXA_PID:-}"    ]] && kill "$EXA_PID"    2>/dev/null || true
    [[ -n "${EXB_PID:-}"    ]] && kill "$EXB_PID"    2>/dev/null || true
}
trap cleanup INT TERM

# 1) Trade bridge
python3 trade_bridge.py &
BRIDGE_PID=$!
echo "[SCENARIO 03] trade_bridge.py PID=$BRIDGE_PID"

# EXA – normal latency, acts like a “fast” venue
python3 exchange_sim.py \
  --exch-id EXA \
  --symbol BTCUSD \
  --base-price 90000.0 \
  --volatility 0.35 \
  --tick-size 0.01 \
  --feed-target-ip "${BBB_IP}" \
  --feed-port "${EXA_FEED_PORT}" \
  --order-port "${EXA_ORDER_PORT}" \
  --fill-target-ip 127.0.0.1 \
  --fill-target-port "${FILL_PORT}" \
  --tick-hz 80.0 \
  --order-latency-us-mean 2500 \
  --order-latency-us-std 600 \
  --feed-latency-us-mean 1800 \
  --feed-latency-us-std 400 &
EXA_PID=$!
echo "[SCENARIO 03] EXA PID=$EXA_PID"

# EXB – richer but very slow, “toxic” venue
python3 exchange_sim.py \
  --exch-id EXB \
  --symbol BTCUSD \
  --base-price 90002.0 \
  --volatility 0.35 \
  --tick-size 0.01 \
  --feed-target-ip "${BBB_IP}" \
  --feed-port "${EXB_FEED_PORT}" \
  --order-port "${EXB_ORDER_PORT}" \
  --fill-target-ip 127.0.0.1 \
  --fill-target-port "${FILL_PORT}" \
  --tick-hz 80.0 \
  --order-latency-us-mean 15000 \
  --order-latency-us-std 4000 \
  --feed-latency-us-mean 6000 \
  --feed-latency-us-std 2000 &
EXB_PID=$!
echo "[SCENARIO 03] EXB PID=$EXB_PID"

echo "[SCENARIO 03] Running. EXB rich BUT slow – expect slippage and potentially negative P&L. Ctrl+C to stop."
wait
