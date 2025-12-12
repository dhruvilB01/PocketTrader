#!/usr/bin/env python3
"""
orderbook.py

Limit order book with:
- Price-time priority
- Limit and market orders
- Partial fills
- Cancels
- Top-of-book query

Side: 'B' (buy) or 'S' (sell)
Type: 'L' (limit) or 'M' (market)
"""

from dataclasses import dataclass
from collections import deque
from typing import Deque, Dict, List, Optional, Tuple


@dataclass
class Order:
    order_id: int
    client_id: str
    side: str         # 'B' or 'S'
    type: str         # 'L' or 'M'
    price: float      # limit price, ignored for market
    qty: float        # original quantity
    remaining: float  # remaining quantity
    ts_ns: int        # receive time


@dataclass
class Trade:
    symbol: str
    price: float
    qty: float
    taker_order_id: int
    maker_order_id: int
    taker_client_id: str
    maker_client_id: str
    ts_ns: int


class OrderBook:
    def __init__(self, symbol: str, tick_size: float = 0.01):
        self.symbol = symbol
        self.tick_size = tick_size

        # price -> deque[Order]
        self._bids: Dict[float, Deque[Order]] = {}
        self._asks: Dict[float, Deque[Order]] = {}

        self._bid_prices: List[float] = []
        self._ask_prices: List[float] = []

        # Quick lookup for cancels
        self._order_index: Dict[int, Order] = {}

        self.last_trade_price: Optional[float] = None

    # ---------- helpers ----------

    def _round_price(self, price: float) -> float:
        if self.tick_size <= 0:
            return price
        ticks = round(price / self.tick_size)
        return ticks * self.tick_size

    def _insert_price_level(self, price_list: List[float], price: float, ascending: bool) -> None:
        if price in price_list:
            return
        price_list.append(price)
        price_list.sort(reverse=not ascending)

    def _cleanup_price_level(self, side_dict: Dict[float, Deque[Order]],
                             price_list: List[float], price: float) -> None:
        q = side_dict.get(price)
        if not q:
            side_dict.pop(price, None)
            if price in price_list:
                price_list.remove(price)

    # ---------- public API ----------

    def best_bid(self) -> Optional[Tuple[float, float]]:
        if not self._bid_prices:
            return None
        best_price = max(self._bid_prices)
        q = self._bids[best_price]
        total_qty = sum(o.remaining for o in q)
        return best_price, total_qty

    def best_ask(self) -> Optional[Tuple[float, float]]:
        if not self._ask_prices:
            return None
        best_price = min(self._ask_prices)
        q = self._asks[best_price]
        total_qty = sum(o.remaining for o in q)
        return best_price, total_qty

    def top_of_book(self) -> Tuple[Optional[float], Optional[float]]:
        bid = self.best_bid()
        ask = self.best_ask()
        best_bid = bid[0] if bid else None
        best_ask = ask[0] if ask else None
        return best_bid, best_ask

    def add_order(self, order: Order) -> List[Trade]:
        """
        Add a new order and match against opposite side.
        Returns list of trades generated.
        """
        if order.side not in ("B", "S"):
            raise ValueError("side must be 'B' or 'S'")
        if order.type not in ("L", "M"):
            raise ValueError("type must be 'L' or 'M'")

        if order.type == "L":
            order.price = self._round_price(order.price)

        trades: List[Trade] = []

        if order.side == "B":
            trades = self._match_buy(order)
            if order.type == "L" and order.remaining > 1e-9:
                self._add_to_book(self._bids, self._bid_prices,
                                  price=order.price, order=order, ascending=False)
        else:
            trades = self._match_sell(order)
            if order.type == "L" and order.remaining > 1e-9:
                self._add_to_book(self._asks, self._ask_prices,
                                  price=order.price, order=order, ascending=True)

        return trades

    def cancel_order(self, order_id: int) -> bool:
        order = self._order_index.get(order_id)
        if not order:
            return False
        if order.remaining <= 1e-9:
            self._order_index.pop(order_id, None)
            return False

        side_dict = self._bids if order.side == "B" else self._asks
        price_list = self._bid_prices if order.side == "B" else self._ask_prices
        price = order.price

        q = side_dict.get(price)
        if not q:
            self._order_index.pop(order_id, None)
            return False

        for o in list(q):
            if o is order:
                q.remove(o)
                self._order_index.pop(order_id, None)
                if not q:
                    self._cleanup_price_level(side_dict, price_list, price)
                return True

        self._order_index.pop(order_id, None)
        return False

    # ---------- internal matching ----------

    def _add_to_book(self,
                     side_dict: Dict[float, Deque[Order]],
                     price_list: List[float],
                     price: float,
                     order: Order,
                     ascending: bool) -> None:
        if price not in side_dict:
            side_dict[price] = deque()
            self._insert_price_level(price_list, price, ascending=ascending)
        side_dict[price].append(order)
        self._order_index[order.order_id] = order

    def _match_buy(self, order: Order) -> List[Trade]:
        trades: List[Trade] = []
        while order.remaining > 1e-9 and self._ask_prices:
            best_ask_price = min(self._ask_prices)
            if order.type == "L" and best_ask_price > order.price + 1e-12:
                break

            level_queue = self._asks[best_ask_price]
            while level_queue and order.remaining > 1e-9:
                resting = level_queue[0]
                trade_qty = min(order.remaining, resting.remaining)
                if trade_qty <= 0:
                    break

                trade_price = best_ask_price
                trade = Trade(
                    symbol=self.symbol,
                    price=trade_price,
                    qty=trade_qty,
                    taker_order_id=order.order_id,
                    maker_order_id=resting.order_id,
                    taker_client_id=order.client_id,
                    maker_client_id=resting.client_id,
                    ts_ns=order.ts_ns,
                )
                trades.append(trade)
                self.last_trade_price = trade_price

                order.remaining -= trade_qty
                resting.remaining -= trade_qty

                if resting.remaining <= 1e-9:
                    level_queue.popleft()
                    self._order_index.pop(resting.order_id, None)

                if order.remaining <= 1e-9:
                    break

            if not level_queue:
                self._cleanup_price_level(self._asks, self._ask_prices, best_ask_price)
            if order.type == "M" and not self._ask_prices:
                break

        return trades

    def _match_sell(self, order: Order) -> List[Trade]:
        trades: List[Trade] = []
        while order.remaining > 1e-9 and self._bid_prices:
            best_bid_price = max(self._bid_prices)
            if order.type == "L" and best_bid_price < order.price - 1e-12:
                break

            level_queue = self._bids[best_bid_price]
            while level_queue and order.remaining > 1e-9:
                resting = level_queue[0]
                trade_qty = min(order.remaining, resting.remaining)
                if trade_qty <= 0:
                    break

                trade_price = best_bid_price
                trade = Trade(
                    symbol=self.symbol,
                    price=trade_price,
                    qty=trade_qty,
                    taker_order_id=order.order_id,
                    maker_order_id=resting.order_id,
                    taker_client_id=order.client_id,
                    maker_client_id=resting.client_id,
                    ts_ns=order.ts_ns,
                )
                trades.append(trade)
                self.last_trade_price = trade_price

                order.remaining -= trade_qty
                resting.remaining -= trade_qty

                if resting.remaining <= 1e-9:
                    level_queue.popleft()
                    self._order_index.pop(resting.order_id, None)

                if order.remaining <= 1e-9:
                    break

            if not level_queue:
                self._cleanup_price_level(self._bids, self._bid_prices, best_bid_price)
            if order.type == "M" and not self._bid_prices:
                break

        return trades
