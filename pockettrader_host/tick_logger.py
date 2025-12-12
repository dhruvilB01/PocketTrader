#!/usr/bin/env python3
"""
tick_logger.py

Listens to EXA and EXB market data feeds and logs ticks to CSV.

Expected message:
    TICK <EXCH_ID> <SYMBOL> <bid> <ask> <seq> <ts_ns>

Outputs:
    exa_ticks.csv
    exb_ticks.csv

Columns:
    ts_ns_host, exch, symbol, bid, ask, seq
"""

import socket
import select
import time
import csv

LISTEN_IP = "0.0.0.0"
EXA_PORT = 6001
EXB_PORT = 6002


def create_udp_socket(port: int) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((LISTEN_IP, port))
    return sock


def main():
    exa_sock = create_udp_socket(EXA_PORT)
    exb_sock = create_udp_socket(EXB_PORT)

    print(f"[TICK_LOGGER] Listening EXA ticks on {LISTEN_IP}:{EXA_PORT}")
    print(f"[TICK_LOGGER] Listening EXB ticks on {LISTEN_IP}:{EXB_PORT}")

    exa_file = open("exa_ticks.csv", "w", newline="")
    exb_file = open("exb_ticks.csv", "w", newline="")

    exa_writer = csv.writer(exa_file)
    exb_writer = csv.writer(exb_file)

    header = ["ts_ns_host", "exch", "symbol", "bid", "ask", "seq"]
    exa_writer.writerow(header)
    exb_writer.writerow(header)
    exa_file.flush()
    exb_file.flush()

    try:
        while True:
            read_socks, _, _ = select.select([exa_sock, exb_sock], [], [], 0.5)

            for s in read_socks:
                try:
                    data, addr = s.recvfrom(4096)
                except OSError:
                    continue

                msg = data.decode("ascii", errors="ignore").strip()
                parts = msg.split()

                if len(parts) != 7 or parts[0].upper() != "TICK":
                    print(f"[TICK_LOGGER] Bad TICK from {addr}: {msg}")
                    continue

                _, exch, symbol, bid_str, ask_str, seq_str, ts_ns_str = parts

                try:
                    bid = float(bid_str)
                    ask = float(ask_str)
                    seq = int(seq_str)
                except ValueError:
                    print(f"[TICK_LOGGER] Parse error: {msg}")
                    continue

                ts_ns_host = time.time_ns()
                exch_up = exch.upper()

                row = [ts_ns_host, exch_up, symbol, bid, ask, seq]

                if s is exa_sock or exch_up == "EXA":
                    exa_writer.writerow(row)
                    exa_file.flush()
                elif s is exb_sock or exch_up == "EXB":
                    exb_writer.writerow(row)
                    exb_file.flush()
                else:
                    # Unknown exchange, ignore
                    continue

    except KeyboardInterrupt:
        print("\n[TICK_LOGGER] Stopped by user")
    finally:
        try:
            exa_file.close()
            exb_file.close()
        except Exception:
            pass
        exa_sock.close()
        exb_sock.close()


if __name__ == "__main__":
    main()
