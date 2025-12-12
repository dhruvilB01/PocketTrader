#!/usr/bin/env python3
"""
plot_arb_pnl.py

Reads arb_log.csv produced by trade_bridge.py and plots:

1) Cumulative P&L vs trade index
2) Histogram of realized spreads
3) P&L per trade vs trade index
"""

import csv
import matplotlib.pyplot as plt


def load_arb_log(path: str = "arb_log.csv"):
    trade_idx = []
    pnl = []
    spreads = []

    with open(path, "r", newline="") as f:
        reader = csv.DictReader(f)
        for i, row in enumerate(reader):
            try:
                pnl_val = float(row["pnl"])
                spread_val = float(row["spread_realized"])
            except (KeyError, ValueError) as e:
                print(f"[PLOT] Skip row {i}: {e}")
                continue

            trade_idx.append(len(trade_idx))
            pnl.append(pnl_val)
            spreads.append(spread_val)

    return trade_idx, pnl, spreads


def compute_cum_pnl(pnl):
    cum = []
    running = 0.0
    for v in pnl:
        running += v
        cum.append(running)
    return cum


def main():
    trade_idx, pnl, spreads = load_arb_log("arb_log.csv")

    if not trade_idx:
        print("[PLOT] No valid rows in arb_log.csv")
        return

    cum_pnl = compute_cum_pnl(pnl)

    # 1) Cumulative P&L
    plt.figure()
    plt.plot(trade_idx, cum_pnl)
    plt.xlabel("Trade index")
    plt.ylabel("Cumulative P&L")
    plt.title("Strategy equity curve (cumulative P&L)")
    plt.grid(True)

    # 2) Histogram of realized spreads
    plt.figure()
    plt.hist(spreads, bins=30)
    plt.xlabel("Realized spread (sell_px - buy_px)")
    plt.ylabel("Frequency")
    plt.title("Distribution of realized spreads")
    plt.grid(True)

    # 3) P&L per trade
    plt.figure()
    plt.bar(trade_idx, pnl)
    plt.xlabel("Trade index")
    plt.ylabel("P&L per trade")
    plt.title("Per-trade P&L")
    plt.grid(True)

    plt.tight_layout()
    plt.show()


if __name__ == "__main__":
    main()
