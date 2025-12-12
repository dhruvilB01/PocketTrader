#!/usr/bin/env python3
"""
exchange_sim.py

Exchange simulator with real order book:

- Maintains limit order book (OrderBook)
- Accepts client orders over UDP:
    NEW <client_id> <order_id> <side> <type> <price> <qty>
    CXL <client_id> <order_id>
- Generates random background order flow
- Publishes top-of-book ticks over UDP (for PocketTrader)
- Publishes fills over UDP to a "bridge" process:

FILL message format (UDP, text):
    FILL <EXCH_ID> <SYMBOL> <price> <qty> <taker_client> <taker_oid>
         <maker_client> <maker_oid> <ts_ns>

IMPORTANT:
- All timestamps sent on the wire (TICK/FILL last field) use time.time_ns()
  so BBB can compute true feed latency with CLOCK_REALTIME, assuming clocks
  are NTP-aligned.
- This version also supports configurable feed and order path latency so that a
  latency-arb trader will sometimes lose money when acting on stale quotes.
"""

import argparse
import random
import socket
import select
import time
from typing import Tuple, List

from orderbook import OrderBook, Order, Trade


class RandomOrderFlow:
    def __init__(self,
                 book: OrderBook,
                 exch_id: str,
                 base_price: float,
                 volatility: float,
                 order_prob: float = 0.4,
                 cancel_prob: float = 0.2,
                 min_qty: float = 0.01,
                 max_qty: float = 0.1):
        self.book = book
        self.exch_id = exch_id
        self.mid_price = base_price
        self.volatility = volatility
        self.order_prob = order_prob
        self.cancel_prob = cancel_prob
        self.min_qty = min_qty
        self.max_qty = max_qty
        self._next_order_id = 1_000_000_000

    def step(self, now_ns: int) -> None:
        # 'now_ns' here is host MONOTONIC time used only for simulation timing / ordering
        self.mid_price += random.gauss(0.0, self.volatility)
        if self.mid_price <= 0:
            self.mid_price = abs(self.mid_price) + 1.0

        # Random new background limit orders
        if random.random() < self.order_prob:
            side = random.choice(["B", "S"])
            spread_half = 1.5
            if side == "B":
                price = self.mid_price - random.random() * spread_half
            else:
                price = self.mid_price + random.random() * spread_half

            qty = random.uniform(self.min_qty, self.max_qty)
            oid = self._next_order_id
            self._next_order_id += 1

            order = Order(
                order_id=oid,
                client_id=f"BG_{self.exch_id}",
                side=side,
                type="L",
                price=price,
                qty=qty,
                remaining=qty,
                ts_ns=now_ns,  # simulation / matching timestamp (monotonic)
            )
            _ = self.book.add_order(order)

        # Random aggressive orders to cross the spread
        if random.random() < self.cancel_prob:
            best_bid = self.book.best_bid()
            best_ask = self.book.best_ask()
            target_side = random.choice(["B", "S"])
            if target_side == "B" and best_bid:
                price, _ = best_bid
                qty = random.uniform(self.min_qty, self.max_qty)
                oid = self._next_order_id
                self._next_order_id += 1
                mkt = Order(
                    order_id=oid,
                    client_id=f"BG_{self.exch_id}",
                    side="S",
                    type="M",
                    price=price,
                    qty=qty,
                    remaining=qty,
                    ts_ns=now_ns,
                )
                self.book.add_order(mkt)
            elif target_side == "S" and best_ask:
                price, _ = best_ask
                qty = random.uniform(self.min_qty, self.max_qty)
                oid = self._next_order_id
                self._next_order_id += 1
                mkt = Order(
                    order_id=oid,
                    client_id=f"BG_{self.exch_id}",
                    side="B",
                    type="M",
                    price=price,
                    qty=qty,
                    remaining=qty,
                    ts_ns=now_ns,
                )
                self.book.add_order(mkt)


class ExchangeSimulator:
    def __init__(self,
                 exch_id: str,
                 symbol: str,
                 base_price: float,
                 volatility: float,
                 tick_size: float,
                 feed_target_ip: str,
                 feed_port: int,
                 order_listen_ip: str,
                 order_port: int,
                 fill_target_ip: str,
                 fill_target_port: int,
                 tick_hz: float = 50.0,
                 order_latency_mean_us: float = 0.0,
                 order_latency_std_us: float = 0.0,
                 feed_latency_mean_us: float = 0.0,
                 feed_latency_std_us: float = 0.0):
        self.exch_id = exch_id
        self.symbol = symbol
        self.seq = 0

        self.book = OrderBook(symbol=symbol, tick_size=tick_size)
        self.rand_flow = RandomOrderFlow(
            book=self.book,
            exch_id=exch_id,
            base_price=base_price,
            volatility=volatility,
        )

        # Latency model (all internal scheduling in MONOTONIC nanoseconds)
        self.order_latency_mean_ns = max(0.0, order_latency_mean_us) * 1000.0
        self.order_latency_std_ns = max(0.0, order_latency_std_us) * 1000.0
        self.feed_latency_mean_ns = max(0.0, feed_latency_mean_us) * 1000.0
        self.feed_latency_std_ns = max(0.0, feed_latency_std_us) * 1000.0

        # Queues of (scheduled_mono_ns, payload)
        self._pending_orders: List[Tuple[int, Order]] = []
        self._pending_ticks: List[Tuple[int, float, float, int]] = []

        # Market data feed socket
        self.feed_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.feed_target = (feed_target_ip, feed_port)

        # FILL feed socket
        self.fill_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.fill_target = (fill_target_ip, fill_target_port)

        # Order entry socket (non-blocking)
        self.order_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.order_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.order_sock.bind((order_listen_ip, order_port))
        self.order_sock.setblocking(False)

        # Use host MONOTONIC for internal pacing
        self.tick_interval_ns = int(1e9 / tick_hz)
        self.last_tick_ns = time.monotonic_ns()
        self.synthetic_mid = base_price

        print(f"[{self.exch_id}] Exchange simulator up "
              f"(symbol={self.symbol}, feed={self.feed_target}, "
              f"orders={order_listen_ip}:{order_port}, "
              f"fills={self.fill_target}, "
              f"order_latency~{order_latency_mean_us}us, "
              f"feed_latency~{feed_latency_mean_us}us)")

    # ---------- core loop ----------

    def run(self) -> None:
        poller_timeout = 0.005

        try:
            while True:
                now_mono_ns = time.monotonic_ns()

                # 1) Pull client messages and schedule orders
                self._process_client_messages(timeout=poller_timeout)

                # 2) Execute any orders whose simulated latency has expired
                self._process_pending_orders(now_mono_ns)

                # 3) Advance background order flow (moves the book even if trader is idle)
                self.rand_flow.step(now_mono_ns)

                # 4) Take a book snapshot for the next TICK
                if now_mono_ns - self.last_tick_ns >= self.tick_interval_ns:
                    self._publish_tick(now_mono_ns)
                    self.last_tick_ns = now_mono_ns

                # 5) Deliver any TICKs whose simulated feed latency has expired
                self._flush_pending_ticks(now_mono_ns)
        except KeyboardInterrupt:
            print(f"[{self.exch_id}] Stopped by user")

    # ---------- client message handling ----------

    def _process_client_messages(self, timeout: float) -> None:
        read_socks, _, _ = select.select([self.order_sock], [], [], timeout)
        for s in read_socks:
            try:
                data, addr = s.recvfrom(4096)
            except BlockingIOError:
                return
            if not data:
                continue
            msg = data.decode("ascii", errors="ignore").strip()
            self._handle_client_message(msg, addr)

    def _schedule_order(self, order: Order, now_mono_ns: int) -> None:
        # Apply a Gaussian latency model, clamped at zero
        if self.order_latency_mean_ns > 0.0 or self.order_latency_std_ns > 0.0:
            jitter = random.gauss(self.order_latency_mean_ns, self.order_latency_std_ns)
            if jitter < 0.0:
                jitter = 0.0
        else:
            jitter = 0.0
        scheduled_ns = now_mono_ns + int(jitter)
        # Use the *arrival* time at the book for the internal timestamp
        order.ts_ns = scheduled_ns
        self._pending_orders.append((scheduled_ns, order))

    def _process_pending_orders(self, now_mono_ns: int) -> None:
        if not self._pending_orders:
            return
        # _pending_orders is append-only with non-decreasing scheduled_ns,
        # so we can process from the front.
        idx = 0
        n = len(self._pending_orders)
        while idx < n:
            scheduled_ns, order = self._pending_orders[idx]
            if scheduled_ns > now_mono_ns:
                break
            trades = self.book.add_order(order)
            for tr in trades:
                self._log_trade(tr)
            idx += 1
        if idx > 0:
            del self._pending_orders[:idx]

    def _handle_client_message(self, msg: str, addr: Tuple[str, int]) -> None:
        parts = msg.split()
        if not parts:
            return

        cmd = parts[0].upper()
        now_mono_ns = time.monotonic_ns()

        try:
            if cmd == "NEW":
                if len(parts) != 7:
                    print(f"[{self.exch_id}] Bad NEW msg: {msg}")
                    return
                client_id = parts[1]
                client_order_id = int(parts[2])
                side = parts[3].upper()
                otype = parts[4].upper()
                price = float(parts[5])
                qty = float(parts[6])

                internal_oid = client_order_id  # simple mapping

                order = Order(
                    order_id=internal_oid,
                    client_id=client_id,
                    side=side,
                    type=otype,
                    price=price,
                    qty=qty,
                    remaining=qty,
                    ts_ns=now_mono_ns,  # will be overwritten in _schedule_order
                )
                self._schedule_order(order, now_mono_ns)

            elif cmd == "CXL":
                if len(parts) != 3:
                    print(f"[{self.exch_id}] Bad CXL msg: {msg}")
                    return
                client_id = parts[1]
                client_order_id = int(parts[2])
                internal_oid = client_order_id
                ok = self.book.cancel_order(internal_oid)
                if not ok:
                    print(f"[{self.exch_id}] Cancel failed for {client_id} {client_order_id}")
            else:
                print(f"[{self.exch_id}] Unknown command: {msg}")
        except Exception as e:
            print(f"[{self.exch_id}] Error handling client msg '{msg}': {e}")

    # ---------- trade logging & FILL feed ----------

    def _log_trade(self, tr: Trade) -> None:
        print(
            f"[{self.exch_id}] TRADE {tr.symbol} {tr.qty:.4f} @ {tr.price:.2f} "
            f"(taker={tr.taker_client_id}:{tr.taker_order_id}, "
            f"maker={tr.maker_client_id}:{tr.maker_order_id})"
        )
        # Use REALTIME for on-wire timestamp so BBB can compute feed latency
        send_ts_ns = time.time_ns()
        msg = (
            f"FILL {self.exch_id} {tr.symbol} {tr.price:.6f} {tr.qty:.6f} "
            f"{tr.taker_client_id} {tr.taker_order_id} "
            f"{tr.maker_client_id} {tr.maker_order_id} {send_ts_ns}"
        )
        try:
            self.fill_sock.sendto(msg.encode("ascii"), self.fill_target)
        except OSError as e:
            print(f"[{self.exch_id}] Error sending FILL: {e}")

    # ---------- market data feed ----------

    def _publish_tick(self, now_mono_ns: int) -> None:
        bid, ask = self.book.top_of_book()

        if bid is None and ask is None:
            mid = self.synthetic_mid
            bid = mid - 0.25
            ask = mid + 0.25
        elif bid is None and ask is not None:
            bid = ask - 0.5
        elif ask is None and bid is not None:
            ask = bid + 0.5

        self.synthetic_mid = (bid + ask) / 2.0
        self.seq += 1

        # Schedule TICK with simulated feed latency.
        if self.feed_latency_mean_ns > 0.0 or self.feed_latency_std_ns > 0.0:
            jitter = random.gauss(self.feed_latency_mean_ns, self.feed_latency_std_ns)
            if jitter < 0.0:
                jitter = 0.0
        else:
            jitter = 0.0
        scheduled_ns = now_mono_ns + int(jitter)
        self._pending_ticks.append((scheduled_ns, bid, ask, self.seq))

    def _flush_pending_ticks(self, now_mono_ns: int) -> None:
        if not self._pending_ticks:
            return
        idx = 0
        n = len(self._pending_ticks)
        while idx < n:
            scheduled_ns, bid, ask, seq = self._pending_ticks[idx]
            if scheduled_ns > now_mono_ns:
                break
            # Use REALTIME for the on-wire timestamp
            send_ts_ns = time.time_ns()
            msg = f"TICK {self.exch_id} {self.symbol} {bid:.2f} {ask:.2f} {seq} {send_ts_ns}"
            try:
                self.feed_sock.sendto(msg.encode("ascii"), self.feed_target)
            except OSError as e:
                print(f"[{self.exch_id}] Error sending TICK: {e}")
            idx += 1
        if idx > 0:
            del self._pending_ticks[:idx]


def main():
    parser = argparse.ArgumentParser(description="Exchange simulator with real order book")
    parser.add_argument("--exch-id", type=str, required=True)
    parser.add_argument("--symbol", type=str, default="BTCUSD")
    parser.add_argument("--base-price", type=float, default=90000.0)
    parser.add_argument("--volatility", type=float, default=1.0)
    parser.add_argument("--tick-size", type=float, default=0.01)

    parser.add_argument("--feed-target-ip", type=str, required=True)
    parser.add_argument("--feed-port", type=int, required=True)

    parser.add_argument("--order-listen-ip", type=str, default="0.0.0.0")
    parser.add_argument("--order-port", type=int, default=9000)

    parser.add_argument("--fill-target-ip", type=str, required=True)
    parser.add_argument("--fill-target-port", type=int, required=True)

    parser.add_argument("--tick-hz", type=float, default=50.0)

    # New: configurable order-path and feed latency (in microseconds)
    parser.add_argument("--order-latency-us-mean", type=float, default=0.0,
                        help="Mean simulated order latency (µs) from trader to book")
    parser.add_argument("--order-latency-us-std", type=float, default=0.0,
                        help="Std-dev of simulated order latency (µs)")
    parser.add_argument("--feed-latency-us-mean", type=float, default=0.0,
                        help="Mean simulated market data latency (µs) from book to trader")
    parser.add_argument("--feed-latency-us-std", type=float, default=0.0,
                        help="Std-dev of simulated market data latency (µs)")

    args = parser.parse_args()

    sim = ExchangeSimulator(
        exch_id=args.exch_id,
        symbol=args.symbol,
        base_price=args.base_price,
        volatility=args.volatility,
        tick_size=args.tick_size,
        feed_target_ip=args.feed_target_ip,
        feed_port=args.feed_port,
        order_listen_ip=args.order_listen_ip,
        order_port=args.order_port,
        fill_target_ip=args.fill_target_ip,
        fill_target_port=args.fill_target_port,
        tick_hz=args.tick_hz,
        order_latency_mean_us=args.order_latency_us_mean,
        order_latency_std_us=args.order_latency_us_std,
        feed_latency_mean_us=args.feed_latency_us_mean,
        feed_latency_std_us=args.feed_latency_us_std,
    )
    sim.run()


if __name__ == "__main__":
    main()



# #!/usr/bin/env python3
# """
# exchange_sim.py

# Exchange simulator with real order book:

# - Maintains limit order book (OrderBook)
# - Accepts client orders over UDP:
#     NEW <client_id> <order_id> <side> <type> <price> <qty>
#     CXL <client_id> <order_id>
# - Generates random background order flow
# - Publishes top-of-book ticks over UDP (for PocketTrader)
# - Publishes fills over UDP to a "bridge" process:

# FILL message format (UDP, text):
#     FILL <EXCH_ID> <SYMBOL> <price> <qty> <taker_client> <taker_oid> <maker_client> <maker_oid> <ts_ns>

# IMPORTANT:
# - All timestamps sent on the wire (TICK/FILL last field) now use time.time_ns()
#   so BBB can compute true feed latency with CLOCK_REALTIME, assuming clocks are NTP-aligned.
# """

# import argparse
# import random
# import socket
# import select
# import time
# from typing import Tuple

# from orderbook import OrderBook, Order, Trade


# class RandomOrderFlow:
#     def __init__(self,
#                  book: OrderBook,
#                  exch_id: str,
#                  base_price: float,
#                  volatility: float,
#                  order_prob: float = 0.4,
#                  cancel_prob: float = 0.2,
#                  min_qty: float = 0.01,
#                  max_qty: float = 0.1):
#         self.book = book
#         self.exch_id = exch_id
#         self.mid_price = base_price
#         self.volatility = volatility
#         self.order_prob = order_prob
#         self.cancel_prob = cancel_prob
#         self.min_qty = min_qty
#         self.max_qty = max_qty
#         self._next_order_id = 1_000_000_000

#     def step(self, now_ns: int) -> None:
#         # 'now_ns' here is host MONOTONIC time used only for simulation timing / ordering
#         self.mid_price += random.gauss(0.0, self.volatility)
#         if self.mid_price <= 0:
#             self.mid_price = abs(self.mid_price) + 1.0

#         if random.random() < self.order_prob:
#             side = random.choice(["B", "S"])
#             spread_half = 1.5
#             if side == "B":
#                 price = self.mid_price - random.random() * spread_half
#             else:
#                 price = self.mid_price + random.random() * spread_half

#             qty = random.uniform(self.min_qty, self.max_qty)
#             oid = self._next_order_id
#             self._next_order_id += 1

#             order = Order(
#                 order_id=oid,
#                 client_id=f"BG_{self.exch_id}",
#                 side=side,
#                 type="L",
#                 price=price,
#                 qty=qty,
#                 remaining=qty,
#                 ts_ns=now_ns,  # simulation / matching timestamp (monotonic)
#             )
#             _ = self.book.add_order(order)

#         if random.random() < self.cancel_prob:
#             best_bid = self.book.best_bid()
#             best_ask = self.book.best_ask()
#             target_side = random.choice(["B", "S"])
#             if target_side == "B" and best_bid:
#                 price, _ = best_bid
#                 qty = random.uniform(self.min_qty, self.max_qty)
#                 oid = self._next_order_id
#                 self._next_order_id += 1
#                 mkt = Order(
#                     order_id=oid,
#                     client_id=f"BG_{self.exch_id}",
#                     side="S",
#                     type="M",
#                     price=price,
#                     qty=qty,
#                     remaining=qty,
#                     ts_ns=now_ns,
#                 )
#                 self.book.add_order(mkt)
#             elif target_side == "S" and best_ask:
#                 price, _ = best_ask
#                 qty = random.uniform(self.min_qty, self.max_qty)
#                 oid = self._next_order_id
#                 self._next_order_id += 1
#                 mkt = Order(
#                     order_id=oid,
#                     client_id=f"BG_{self.exch_id}",
#                     side="B",
#                     type="M",
#                     price=price,
#                     qty=qty,
#                     remaining=qty,
#                     ts_ns=now_ns,
#                 )
#                 self.book.add_order(mkt)


# class ExchangeSimulator:
#     def __init__(self,
#                  exch_id: str,
#                  symbol: str,
#                  base_price: float,
#                  volatility: float,
#                  tick_size: float,
#                  feed_target_ip: str,
#                  feed_port: int,
#                  order_listen_ip: str,
#                  order_port: int,
#                  fill_target_ip: str,
#                  fill_target_port: int,
#                  tick_hz: float = 50.0):
#         self.exch_id = exch_id
#         self.symbol = symbol
#         self.seq = 0

#         self.book = OrderBook(symbol=symbol, tick_size=tick_size)
#         self.rand_flow = RandomOrderFlow(
#             book=self.book,
#             exch_id=exch_id,
#             base_price=base_price,
#             volatility=volatility,
#         )

#         # Market data feed socket
#         self.feed_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
#         self.feed_target = (feed_target_ip, feed_port)

#         # FILL feed socket
#         self.fill_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
#         self.fill_target = (fill_target_ip, fill_target_port)

#         # Order entry socket (non-blocking)
#         self.order_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
#         self.order_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
#         self.order_sock.bind((order_listen_ip, order_port))
#         self.order_sock.setblocking(False)

#         # Use host MONOTONIC for internal pacing
#         self.tick_interval_ns = int(1e9 / tick_hz)
#         self.last_tick_ns = time.monotonic_ns()
#         self.synthetic_mid = base_price

#         print(f"[{self.exch_id}] Exchange simulator up "
#               f"(symbol={self.symbol}, feed={self.feed_target}, "
#               f"orders={order_listen_ip}:{order_port}, "
#               f"fills={self.fill_target})")

#     # ---------- core loop ----------

#     def run(self) -> None:
#         poller_timeout = 0.005

#         try:
#             while True:
#                 now_mono_ns = time.monotonic_ns()

#                 self._process_client_messages(timeout=poller_timeout)
#                 self.rand_flow.step(now_mono_ns)

#                 if now_mono_ns - self.last_tick_ns >= self.tick_interval_ns:
#                     self._publish_tick(now_mono_ns)
#                     self.last_tick_ns = now_mono_ns
#         except KeyboardInterrupt:
#             print(f"[{self.exch_id}] Stopped by user")

#     # ---------- client message handling ----------

#     def _process_client_messages(self, timeout: float) -> None:
#         read_socks, _, _ = select.select([self.order_sock], [], [], timeout)
#         for s in read_socks:
#             try:
#                 data, addr = s.recvfrom(4096)
#             except BlockingIOError:
#                 return
#             if not data:
#                 continue
#             msg = data.decode("ascii", errors="ignore").strip()
#             self._handle_client_message(msg, addr)

#     def _handle_client_message(self, msg: str, addr: Tuple[str, int]) -> None:
#         parts = msg.split()
#         if not parts:
#             return

#         cmd = parts[0].upper()
#         now_mono_ns = time.monotonic_ns()

#         try:
#             if cmd == "NEW":
#                 if len(parts) != 7:
#                     print(f"[{self.exch_id}] Bad NEW msg: {msg}")
#                     return
#                 client_id = parts[1]
#                 client_order_id = int(parts[2])
#                 side = parts[3].upper()
#                 otype = parts[4].upper()
#                 price = float(parts[5])
#                 qty = float(parts[6])

#                 internal_oid = client_order_id  # simple mapping

#                 order = Order(
#                     order_id=internal_oid,
#                     client_id=client_id,
#                     side=side,
#                     type=otype,
#                     price=price,
#                     qty=qty,
#                     remaining=qty,
#                     ts_ns=now_mono_ns,  # simulation time
#                 )
#                 trades = self.book.add_order(order)
#                 for tr in trades:
#                     self._log_trade(tr)

#             elif cmd == "CXL":
#                 if len(parts) != 3:
#                     print(f"[{self.exch_id}] Bad CXL msg: {msg}")
#                     return
#                 client_id = parts[1]
#                 client_order_id = int(parts[2])
#                 internal_oid = client_order_id
#                 ok = self.book.cancel_order(internal_oid)
#                 if not ok:
#                     print(f"[{self.exch_id}] Cancel failed for {client_id} {client_order_id}")
#             else:
#                 print(f"[{self.exch_id}] Unknown command: {msg}")
#         except Exception as e:
#             print(f"[{self.exch_id}] Error handling client msg '{msg}': {e}")

#     # ---------- trade logging & FILL feed ----------

#     def _log_trade(self, tr: Trade) -> None:
#         print(
#             f"[{self.exch_id}] TRADE {tr.symbol} {tr.qty:.4f} @ {tr.price:.2f} "
#             f"(taker={tr.taker_client_id}:{tr.taker_order_id}, "
#             f"maker={tr.maker_client_id}:{tr.maker_order_id})"
#         )
#         # Use REALTIME for on-wire timestamp so BBB can compute feed latency
#         send_ts_ns = time.time_ns()
#         msg = (
#             f"FILL {self.exch_id} {tr.symbol} {tr.price:.6f} {tr.qty:.6f} "
#             f"{tr.taker_client_id} {tr.taker_order_id} "
#             f"{tr.maker_client_id} {tr.maker_order_id} {send_ts_ns}"
#         )
#         try:
#             self.fill_sock.sendto(msg.encode("ascii"), self.fill_target)
#         except OSError as e:
#             print(f"[{self.exch_id}] Error sending FILL: {e}")

#     # ---------- market data feed ----------

#     def _publish_tick(self, now_mono_ns: int) -> None:
#         bid, ask = self.book.top_of_book()

#         if bid is None and ask is None:
#             mid = self.synthetic_mid
#             bid = mid - 0.25
#             ask = mid + 0.25
#         elif bid is None and ask is not None:
#             bid = ask - 0.5
#         elif ask is None and bid is not None:
#             ask = bid + 0.5

#         self.synthetic_mid = (bid + ask) / 2.0

#         self.seq += 1

#         # Use REALTIME for TICK timestamp for BBB feed latency
#         send_ts_ns = time.time_ns()
#         msg = f"TICK {self.exch_id} {self.symbol} {bid:.2f} {ask:.2f} {self.seq} {send_ts_ns}"
#         try:
#             self.feed_sock.sendto(msg.encode("ascii"), self.feed_target)
#         except OSError as e:
#             print(f"[{self.exch_id}] Error sending TICK: {e}")


# def main():
#     parser = argparse.ArgumentParser(description="Exchange simulator with real order book")
#     parser.add_argument("--exch-id", type=str, required=True)
#     parser.add_argument("--symbol", type=str, default="BTCUSD")
#     parser.add_argument("--base-price", type=float, default=90000.0)
#     parser.add_argument("--volatility", type=float, default=1.0)
#     parser.add_argument("--tick-size", type=float, default=0.01)

#     parser.add_argument("--feed-target-ip", type=str, required=True)
#     parser.add_argument("--feed-port", type=int, required=True)

#     parser.add_argument("--order-listen-ip", type=str, default="0.0.0.0")
#     parser.add_argument("--order-port", type=int, default=9000)

#     parser.add_argument("--fill-target-ip", type=str, required=True)
#     parser.add_argument("--fill-target-port", type=int, required=True)

#     parser.add_argument("--tick-hz", type=float, default=50.0)

#     args = parser.parse_args()

#     sim = ExchangeSimulator(
#         exch_id=args.exch_id,
#         symbol=args.symbol,
#         base_price=args.base_price,
#         volatility=args.volatility,
#         tick_size=args.tick_size,
#         feed_target_ip=args.feed_target_ip,
#         feed_port=args.feed_port,
#         order_listen_ip=args.order_listen_ip,
#         order_port=args.order_port,
#         fill_target_ip=args.fill_target_ip,
#         fill_target_port=args.fill_target_port,
#         tick_hz=args.tick_hz,
#     )
#     sim.run()


# if __name__ == "__main__":
#     main()
