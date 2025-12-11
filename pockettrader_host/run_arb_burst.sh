#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

source ./env_pockettrader.sh

echo "[ARB BURST] BBB_IP=$BBB_IP"

cleanup() {
    echo "[ARB BURST] Cleaning up..."
    [[ -n "${BRIDGE_PID:-}" ]] && kill "$BRIDGE_PID" 2>/dev/null || true
    [[ -n "${EXA_PID:-}" ]] && kill "$EXA_PID" 2>/dev/null || true
    [[ -n "${EXB_PID:-}" ]] && kill "$EXB_PID" 2>/dev/null || true
}
trap cleanup INT TERM

python3 trade_bridge.py &
BRIDGE_PID=$!
echo "[ARB BURST] trade_bridge.py PID=$BRIDGE_PID"

python3 exchange_sim.py \
  --exch-id EXA \
  --symbol BTCUSD \
  --base-price "${EXA_BASE}" \
  --volatility "${VOL_LOW}" \
  --tick-size 0.01 \
  --feed-target-ip "${BBB_IP}" \
  --feed-port "${EXA_FEED_PORT}" \
  --order-port "${EXA_ORDER_PORT}" \
  --fill-target-ip 127.0.0.1 \
  --fill-target-port "${FILL_PORT}" &
EXA_PID=$!
echo "[ARB BURST] EXA PID=$EXA_PID"

# EXB clearly richer -> frequent arb
python3 exchange_sim.py \
  --exch-id EXB \
  --symbol BTCUSD \
  --base-price "${EXB_BASE_BURST}" \
  --volatility "${VOL_LOW}" \
  --tick-size 0.01 \
  --feed-target-ip "${BBB_IP}" \
  --feed-port "${EXB_FEED_PORT}" \
  --order-port "${EXB_ORDER_PORT}" \
  --fill-target-ip 127.0.0.1 \
  --fill-target-port "${FILL_PORT}" &
EXB_PID=$!
echo "[ARB BURST] EXB PID=$EXB_PID"

echo "[ARB BURST] Running. Expect a lot of trades. Ctrl+C to stop."
wait
