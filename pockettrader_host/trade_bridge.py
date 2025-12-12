#!/usr/bin/env python3
"""
trade_bridge.py

Bridges PocketTrader (BBB) and the exchange simulators.

- Listens for TRADE messages from BBB:
    TRADE <strategy_id> <legA_exch> <legA_side> <legA_price>
          <legB_exch> <legB_side> <legB_price> <size> <spread> <ts_ns>

- Sends real client orders into exchanges:
    NEW PT <order_id> <side> L <price> <qty>

- Listens for FILL messages from exchanges:
    FILL <EXCH_ID> <SYMBOL> <price> <qty>
         <taker_client> <taker_oid> <maker_client> <maker_oid> <ts_ns>

- Aggregates fills per arbitrage and prints realized P&L when both legs filled.
- Logs each completed arbitrage to arb_log.csv for P&L / spread analysis.
"""

import socket
import select
import time
import csv
import os
from dataclasses import dataclass
from typing import Dict, Tuple

TRADE_LISTEN_IP = "0.0.0.0"
TRADE_LISTEN_PORT = 7000  # must match BBB trade_port

FILL_LISTEN_IP = "0.0.0.0"
FILL_LISTEN_PORT = 7100  # must match exchange_sim --fill-target-port

ORDER_TARGET_HOST = "127.0.0.1"
ORDER_PORTS = {
    "EXA": 9101,
    "EXB": 9102,
}

CLIENT_ID = "PT"


@dataclass
class LegState:
    exch: str
    side: str        # "BUY" or "SELL"
    target_qty: float
    filled_qty: float
    weighted_price_sum: float


@dataclass
class ArbState:
    arb_id: int
    legs: Dict[str, LegState]  # "A" / "B"
    closed: bool = False


class TradeBridge:
    def __init__(self):
        # TRADE listener (BBB -> bridge)
        self.trade_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.trade_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.trade_sock.bind((TRADE_LISTEN_IP, TRADE_LISTEN_PORT))

        # FILL listener (exchanges -> bridge)
        self.fill_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.fill_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.fill_sock.bind((FILL_LISTEN_IP, FILL_LISTEN_PORT))

        # Order send socket (shared)
        self.order_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        # Order and arb tracking
        self.next_arb_id = 1
        self.next_order_id = 1

        # (exch, order_id) -> (arb_id, leg_key)
        self.order_to_arb: Dict[Tuple[str, int], Tuple[int, str]] = {}
        self.arbs: Dict[int, ArbState] = {}

        # CSV log for realized arbitrages
        self.arb_log = open("arb_log.csv", "w", newline="")
        self.arb_writer = csv.writer(self.arb_log)
        self.arb_writer.writerow([
            "arb_id",
            "timestamp_iso",
            "size",
            "buy_px",
            "sell_px",
            "spread_realized",
            "pnl",
        ])
        self.arb_log.flush()

        print(f"[BRIDGE] Listening TRADE on {TRADE_LISTEN_IP}:{TRADE_LISTEN_PORT}")
        print(f"[BRIDGE] Listening FILL  on {FILL_LISTEN_IP}:{FILL_LISTEN_PORT}")

    # ---------- main loop ----------

    def run(self):
        try:
            while True:
                read_socks, _, _ = select.select(
                    [self.trade_sock, self.fill_sock], [], [], 0.1
                )
                for s in read_socks:
                    if s is self.trade_sock:
                        data, addr = s.recvfrom(4096)
                        msg = data.decode("ascii", errors="ignore").strip()
                        self.handle_trade_msg(msg, addr)
                    elif s is self.fill_sock:
                        data, addr = s.recvfrom(4096)
                        msg = data.decode("ascii", errors="ignore").strip()
                        self.handle_fill_msg(msg, addr)
        except KeyboardInterrupt:
            print("[BRIDGE] Stopped by user")
        finally:
            try:
                self.arb_log.close()
            except Exception:
                pass

    # ---------- TRADE handling (BBB -> bridge) ----------

    def handle_trade_msg(self, msg: str, addr):
        parts = msg.split()
        if len(parts) != 11 or parts[0].upper() != "TRADE":
            print(f"[BRIDGE] Bad TRADE msg from {addr}: {msg}")
            return

        (
            _,
            strategy_id,
            legA_exch,
            legA_side,
            legA_price_str,
            legB_exch,
            legB_side,
            legB_price_str,
            size_str,
            spread_str,
            ts_ns_str,
        ) = parts

        try:
            legA_price = float(legA_price_str)
            legB_price = float(legB_price_str)
            size = float(size_str)
            spread = float(spread_str)
        except ValueError:
            print(f"[BRIDGE] TRADE parse error: {msg}")
            return

        arb_id = self.next_arb_id
        self.next_arb_id += 1

        print(
            f"[BRIDGE] TRADE#{arb_id} from BBB: "
            f"{legA_exch} {legA_side} {legA_price} / "
            f"{legB_exch} {legB_side} {legB_price}, "
            f"size={size}, spread={spread}"
        )

        legA = LegState(
            exch=legA_exch,
            side=legA_side,
            target_qty=size,
            filled_qty=0.0,
            weighted_price_sum=0.0,
        )
        legB = LegState(
            exch=legB_exch,
            side=legB_side,
            target_qty=size,
            filled_qty=0.0,
            weighted_price_sum=0.0,
        )
        self.arbs[arb_id] = ArbState(arb_id=arb_id, legs={"A": legA, "B": legB})

        # Send actual orders into exchanges
        self._send_leg_order(arb_id, "A", legA, legA_price, size)
        self._send_leg_order(arb_id, "B", legB, legB_price, size)

    def _send_leg_order(self, arb_id: int, leg_key: str,
                        leg: LegState, price: float, qty: float) -> None:
        exch = leg.exch.upper()
        port = ORDER_PORTS.get(exch)
        if port is None:
            print(f"[BRIDGE] Unknown exchange '{exch}' for arb#{arb_id}")
            return

        order_id = self.next_order_id
        self.next_order_id += 1

        # Map BUY/SELL -> B/S for the exchange
        if leg.side.upper() == "BUY":
            side_char = "B"
        elif leg.side.upper() == "SELL":
            side_char = "S"
        else:
            print(f"[BRIDGE] Invalid side {leg.side} for arb#{arb_id}")
            return

        msg = f"NEW {CLIENT_ID} {order_id} {side_char} L {price:.6f} {qty:.6f}"
        self.order_sock.sendto(
            msg.encode("ascii"),
            (ORDER_TARGET_HOST, port),
        )

        self.order_to_arb[(exch, order_id)] = (arb_id, leg_key)

        print(
            f"[BRIDGE] Sent order EXCH={exch} OID={order_id} "
            f"LEG={leg_key} SIDE={side_char} PX={price:.2f} QTY={qty:.4f}"
        )

    # ---------- FILL handling (exchange -> bridge) ----------

    def handle_fill_msg(self, msg: str, addr):
        parts = msg.split()
        if len(parts) != 10 or parts[0].upper() != "FILL":
            print(f"[BRIDGE] Bad FILL msg from {addr}: {msg}")
            return

        (
            _,
            exch_id,
            symbol,
            price_str,
            qty_str,
            taker_client,
            taker_oid_str,
            maker_client,
            maker_oid_str,
            ts_ns_str,
        ) = parts

        try:
            price = float(price_str)
            qty = float(qty_str)
            taker_oid = int(taker_oid_str)
            maker_oid = int(maker_oid_str)
        except ValueError:
            print(f"[BRIDGE] FILL parse error: {msg}")
            return

        exch_id = exch_id.upper()

        # We only care about fills for our client_id
        candidates = []
        if taker_client == CLIENT_ID:
            candidates.append((taker_oid, "taker"))
        if maker_client == CLIENT_ID:
            candidates.append((maker_oid, "maker"))

        if not candidates:
            return  # fill between other participants

        for oid, role in candidates:
            key = (exch_id, oid)
            mapping = self.order_to_arb.get(key)
            if not mapping:
                print(f"[BRIDGE] FILL for unknown order {exch_id}:{oid}: {msg}")
                continue

            arb_id, leg_key = mapping
            arb = self.arbs.get(arb_id)
            if not arb or arb.closed:
                continue

            leg = arb.legs[leg_key]
            leg.filled_qty += qty
            leg.weighted_price_sum += price * qty

            avg_price = leg.weighted_price_sum / max(leg.filled_qty, 1e-12)

            print(
                f"[BRIDGE] FILL arb#{arb_id} LEG={leg_key} "
                f"EXCH={exch_id} ROLE={role} "
                f"px={price:.2f} qty={qty:.4f} "
                f"filled={leg.filled_qty:.4f} avg_px={avg_price:.4f}"
            )

            self._maybe_finalize_arb(arb_id)

    def _maybe_finalize_arb(self, arb_id: int) -> None:
        arb = self.arbs.get(arb_id)
        if not arb or arb.closed:
            return

        legA = arb.legs["A"]
        legB = arb.legs["B"]

        # Require both legs fully filled
        if legA.filled_qty + 1e-9 < legA.target_qty:
            return
        if legB.filled_qty + 1e-9 < legB.target_qty:
            return

        avgA = legA.weighted_price_sum / legA.filled_qty
        avgB = legB.weighted_price_sum / legB.filled_qty

        # Identify which leg is buy / sell
        if legA.side.upper() == "BUY":
            buy_px, sell_px = avgA, avgB
        else:
            buy_px, sell_px = avgB, avgA

        size = min(legA.filled_qty, legB.filled_qty)
        spread_realized = sell_px - buy_px
        pnl = spread_realized * size

        arb.closed = True

        print(
            f"[ARB] DONE #{arb_id} size={size:.4f} "
            f"buy={buy_px:.2f} sell={sell_px:.2f} "
            f"spread_realized={spread_realized:.4f} pnl={pnl:.4f}"
        )

        # Log completed arbitrage to CSV
        if hasattr(self, "arb_writer"):
            ts_iso = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
            self.arb_writer.writerow([
                arb_id,
                ts_iso,
                size,
                buy_px,
                sell_px,
                spread_realized,
                pnl,
            ])
            self.arb_log.flush()


def main():
    bridge = TradeBridge()
    bridge.run()


if __name__ == "__main__":
    main()
