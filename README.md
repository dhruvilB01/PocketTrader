# PocketTrader – README

PocketTrader is a lightweight end-to-end arbitrage trading demo built for the BeagleBone Black (AM3358). It includes a real-time arbitrage engine, a host-side exchange simulator, a trade execution bridge, and an LCD dashboard built with Qt.

## System Overview
PocketTrader runs in three parts:

1. **Arbitrage Engine (BBB)**  
   Located in `pockettrader_core_user_space/`  
   - Receives EXA/EXB TICK data via UDP  
   - Computes spreads and issues TRADE messages  
   - Tracks P&L, latency, rate limits, and risk flags  
   - Writes all state into a POSIX shared-memory block (`/pockettrader_shm`)

2. **Host-Side Environment**  
   Located in `pockettrader_host/`  
   - `exchange_sim.py`: order book simulators for EXA and EXB  
   - `trade_bridge.py`: receives TRADE messages from BBB and submits exchange orders  
   - `run_normal.sh` / `run_arb_burst.sh`: start full test environments  
   - `plot_arb_pnl.py`: visualize spreads and P&L

3. **LCD Dashboard (Qt GUI on BBB)**  
   Located in `pockettrader_gui/`  
   - Displays quotes, spreads, tick-to-trade latency, P&L, and risk indicators  
   - Reads shared memory and updates via a periodic timer  
   - Includes controls such as circuit breaker reset

## How to Run

### 1. Start the simulated markets (on host)
```bash
cd pockettrader_host
./run_normal.sh
```

### 2. Run the arbitrage engine (on BBB)
```bash
cd pockettrader_core_user_space
./pockettrader_core --exa-port 6001 --exb-port 6002 --trade-port 7000
```

### 3. Start the GUI (on BBB)
```bash
cd pockettrader_gui
./pockettrader_gui
```

## Key Features
- Real-time arbitrage between two synthetic exchanges  
- Tick-to-trade latency measurement  
- P&L, drawdown, and risk-flag tracking  
- Stress-test scenario (`run_arb_burst.sh`)  
- Shared-memory interface for fast GUI updates  
- End-to-end trade life cycle (TICK → DECISION → TRADE → FILL → P&L)

## Repository Structure
```
PocketTrader/
 ├── pockettrader_core_user_space/   # Arbitrage engine (C)
 ├── pockettrader_gui/               # Qt dashboard
 └── pockettrader_host/              # Exchange sims + execution bridge
```

## Dependencies
**BBB / Core / GUI**
- gcc / g++  
- pthreads  
- POSIX shared memory  
- Qt5 (widgets)

**Host**
- Python 3.9+  
- No external libraries required

## Notes
- All communication uses UDP for simplicity; no authentication or encryption is implemented.  
- The system is designed for education/testing and is not suitable for live trading environments.
