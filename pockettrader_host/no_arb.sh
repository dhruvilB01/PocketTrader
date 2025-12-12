#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

source ./env_pockettrader.sh

echo "[SCENARIO 01] BBB_IP=$BBB_IP (Efficient / No-Arb)"

cleanup() {
    echo "[SCENARIO 01] Cleaning up..."
    [[ -n "${BRIDGE_PID:-}" ]] && kill "$BRIDGE_PID" 2>/dev/null || true
    [[ -n "${EXA_PID:-}"    ]] && kill "$EXA_PID"    2>/dev/null || true
    [[ -n "${EXB_PID:-}"    ]] && kill "$EXB_PID"    2>/dev/null || true
}
trap cleanup INT TERM

# 1) Trade bridge
python3 trade_bridge.py &
BRIDGE_PID=$!
echo "[SCENARIO 01] trade_bridge.py PID=$BRIDGE_PID"

# EXA – baseline fair market
python3 exchange_sim.py \
  --exch-id EXA \
  --symbol BTCUSD \
  --base-price 90000.0 \
  --volatility 0.20 \
  --tick-size 0.01 \
  --feed-target-ip "${BBB_IP}" \
  --feed-port "${EXA_FEED_PORT}" \
  --order-port "${EXA_ORDER_PORT}" \
  --fill-target-ip 127.0.0.1 \
  --fill-target-port "${FILL_PORT}" \
  --tick-hz 60.0 \
  --order-latency-us-mean 2500 \
  --order-latency-us-std 500 \
  --feed-latency-us-mean 1800 \
  --feed-latency-us-std 300 &
EXA_PID=$!
echo "[SCENARIO 01] EXA PID=$EXA_PID"

# EXB – almost identical to EXA, tiny bias only
python3 exchange_sim.py \
  --exch-id EXB \
  --symbol BTCUSD \
  --base-price 90000.1 \
  --volatility 0.20 \
  --tick-size 0.01 \
  --feed-target-ip "${BBB_IP}" \
  --feed-port "${EXB_FEED_PORT}" \
  --order-port "${EXB_ORDER_PORT}" \
  --fill-target-ip 127.0.0.1 \
  --fill-target-port "${FILL_PORT}" \
  --tick-hz 60.0 \
  --order-latency-us-mean 2600 \
  --order-latency-us-std 500 \
  --feed-latency-us-mean 1900 \
  --feed-latency-us-std 300 &
EXB_PID=$!
echo "[SCENARIO 01] EXB PID=$EXB_PID"

echo "[SCENARIO 01] Running. Expect almost NO trades unless min spread is tiny. Ctrl+C to stop."
wait
