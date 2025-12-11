#ifndef POCKETTRADER_STATE_H
#define POCKETTRADER_STATE_H

#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double bid;
    double ask;
    uint64_t seq;
    uint64_t last_update_ns;   // BBB receive time
    int connected;             // 1 if quote is fresh
} ExchangeQuote;

typedef struct {
    // Latest quotes
    ExchangeQuote exa;
    ExchangeQuote exb;

    // Strategy parameters (modifiable by GUI)
    double min_spread;         // Threshold for trading
    int strategy_mode;         // 0=OFF,1=MONITOR,2=PAPER
    int kill_switch;           // 1 = stop trading
    double trade_size;         // Position size in BTC

    // Metrics
    double last_spread_exa_to_exb;
    double last_spread_exb_to_exa;
    uint64_t last_trade_ts_ns;
    double cumulative_pnl;
    uint32_t trades_count;

    // Latency stats (ns)
    uint64_t last_tick_latency_exa_ns;
    uint64_t last_tick_latency_exb_ns;
    uint64_t avg_tick_latency_exa_ns;
    uint64_t avg_tick_latency_exb_ns;

    // Tick-to-trade latency (ns)
    uint64_t last_tick_to_trade_ns;

    // Safety flags
    int circuit_tripped;       // 1 if P&L limit breached
    int rate_limited;          // 1 if trade skipped due to rate limit

    // -------- Performance metrics --------
    double last_trade_pnl;     // PnL of the most recent trade
    double gross_profit;       // Sum of positive trade PnL
    double gross_loss;         // Sum of absolute value of negative trade PnL
    uint32_t winning_trades;   // Count of trades with pnl >= 0
    uint32_t losing_trades;    // Count of trades with pnl < 0
    double equity_high;        // Running max of cumulative_pnl
    double max_drawdown;       // Most negative (cumulative_pnl - equity_high)
} PocketTraderState;

// Shared memory wrapper: mutex + state in one region so GUI and core can share it.
typedef struct {
    uint32_t magic;           // Magic value to signal initialization
    pthread_mutex_t mutex;    // Process-shared mutex protecting state
    PocketTraderState state;  // Shared state
} PocketTraderShared;

#define POCKETTRADER_SHM_NAME  "/pockettrader_shm"
#define POCKETTRADER_SHM_MAGIC 0x504b5452u  // 'PKTR'

#ifdef __cplusplus
}
#endif

#endif // POCKETTRADER_STATE_H
