#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sched.h>

#include "pockettrader_state.h"

// ---------------- CONFIG ----------------

#define DEFAULT_EXA_PORT      6001
#define DEFAULT_EXB_PORT      6002
#define DEFAULT_TRADE_PORT    7000

#define STALE_THRESHOLD_NS    500000000ULL   // 0.5 seconds
#define MAX_TRADES_PER_SECOND 20
#define P_L_LIMIT             (-100.0)       // Demo P&L circuit breaker

// Turn this to 1 only when debugging – it kills throughput.
#define DEBUG_TICKS           0

static volatile sig_atomic_t g_running = 1;

// Shared memory pointer
static PocketTraderShared *g_shared = NULL;

// Latency log
static FILE *g_latency_log = NULL;

// Trade socket and address
static int g_trade_sock = -1;
static struct sockaddr_in g_trade_addr;
static pthread_mutex_t g_trade_addr_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_trade_addr_ready = 0;

// Core configuration
typedef struct {
    int exa_port;
    int exb_port;
    int trade_port;
} CoreConfig;

// ------------- UTILS -------------

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

// Monotonic time in ns
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Simple EMA helper
static uint64_t ema_ns(uint64_t old_avg, uint64_t sample) {
    if (old_avg == 0) return sample;
    double a = 0.1;
    return (uint64_t)((1.0 - a) * (double)old_avg + a * (double)sample);
}

// Shared memory init
static void init_shared_memory(void) {
    int fd;
    int created = 0;

    fd = shm_open(POCKETTRADER_SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0666);
    if (fd >= 0) {
        created = 1;
        if (ftruncate(fd, sizeof(PocketTraderShared)) < 0) {
            perror("ftruncate shared memory");
            close(fd);
            exit(1);
        }
    } else {
        if (errno != EEXIST) {
            perror("shm_open");
            exit(1);
        }
        fd = shm_open(POCKETTRADER_SHM_NAME, O_RDWR, 0666);
        if (fd < 0) {
            perror("shm_open existing");
            exit(1);
        }
    }

    g_shared = (PocketTraderShared *)mmap(NULL,
                                          sizeof(PocketTraderShared),
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED,
                                          fd,
                                          0);
    if (g_shared == MAP_FAILED) {
        perror("mmap shared");
        close(fd);
        exit(1);
    }
    close(fd);

    if (created) {
        memset(g_shared, 0, sizeof(*g_shared));

        pthread_mutexattr_t attr;
        if (pthread_mutexattr_init(&attr) != 0) {
            perror("pthread_mutexattr_init");
            exit(1);
        }
        if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
            perror("pthread_mutexattr_setpshared");
            pthread_mutexattr_destroy(&attr);
            exit(1);
        }
        if (pthread_mutex_init(&g_shared->mutex, &attr) != 0) {
            perror("pthread_mutex_init");
            pthread_mutexattr_destroy(&attr);
            exit(1);
        }
        pthread_mutexattr_destroy(&attr);

        g_shared->magic = POCKETTRADER_SHM_MAGIC;

        if (pthread_mutex_lock(&g_shared->mutex) == 0) {
            PocketTraderState *st = &g_shared->state;
            memset(st, 0, sizeof(*st));
            st->min_spread    = 0.10;  // default threshold
            st->strategy_mode = 2;     // PAPER
            st->trade_size    = 0.01;  // 0.01 BTC
            pthread_mutex_unlock(&g_shared->mutex);
        }
    } else {
        while (g_shared->magic != POCKETTRADER_SHM_MAGIC) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        }
    }
}

// ---------- Feed receiver thread ----------

typedef struct {
    int sock;
    int is_exa;      // 1 = EXA, 0 = EXB
    CoreConfig *config;
} FeedThreadArgs;

static void *feed_receiver_thread(void *arg) {
    FeedThreadArgs *fta = (FeedThreadArgs *)arg;
    int sock   = fta->sock;
    int is_exa = fta->is_exa;

    char buf[256];
    struct sockaddr_in src_addr;
    socklen_t addrlen = sizeof(src_addr);

    while (g_running) {
        ssize_t n = recvfrom(sock,
                             buf,
                             sizeof(buf) - 1,
                             0,
                             (struct sockaddr *)&src_addr,
                             &addrlen);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recvfrom feed");
            break;
        }
        if (n == 0) {
            continue;
        }

        buf[n] = '\0';

        // Expected: TICK EXA BTCUSD <bid> <ask> <seq> <ts_ns>
        char exch[8]    = {0};
        char symbol[16] = {0};
        double bid      = 0.0;
        double ask      = 0.0;
        unsigned long long seq_ull   = 0;
        unsigned long long ts_ns_ull = 0;

        int scanned = sscanf(buf,
                             "TICK %7s %15s %lf %lf %llu %llu",
                             exch,
                             symbol,
                             &bid,
                             &ask,
                             &seq_ull,
                             &ts_ns_ull);
        if (scanned < 6) {
            fprintf(stderr, "Bad TICK message: %s\n", buf);
            continue;
        }

#if DEBUG_TICKS
        fprintf(stdout,
                "[%s] TICK %s bid=%.2f ask=%.2f seq=%llu\n",
                is_exa ? "EXA" : "EXB",
                symbol,
                bid,
                ask,
                (unsigned long long)seq_ull);
        fflush(stdout);
#endif

        uint64_t t_recv = now_ns();  // BBB receive time (monotonic)

        if (pthread_mutex_lock(&g_shared->mutex) == 0) {
            PocketTraderState *st = &g_shared->state;
            ExchangeQuote *q = is_exa ? &st->exa : &st->exb;

            uint64_t interval_ns = 0;
            if (q->last_update_ns != 0 && t_recv > q->last_update_ns) {
                interval_ns = t_recv - q->last_update_ns;
            }

            q->bid            = bid;
            q->ask            = ask;
            q->seq            = (uint64_t)seq_ull;
            q->last_update_ns = t_recv;
            q->connected      = 1;

            if (interval_ns > 0) {
                if (is_exa) {
                    st->last_tick_latency_exa_ns = interval_ns;
                    st->avg_tick_latency_exa_ns  =
                        ema_ns(st->avg_tick_latency_exa_ns, interval_ns);
                } else {
                    st->last_tick_latency_exb_ns = interval_ns;
                    st->avg_tick_latency_exb_ns  =
                        ema_ns(st->avg_tick_latency_exb_ns, interval_ns);
                }
            }

            pthread_mutex_unlock(&g_shared->mutex);
        }

        // Initialize trade target IP from first packet
        if (!g_trade_addr_ready) {
            if (pthread_mutex_lock(&g_trade_addr_mutex) == 0) {
                if (!g_trade_addr_ready) {
                    memset(&g_trade_addr, 0, sizeof(g_trade_addr));
                    g_trade_addr.sin_family = AF_INET;
                    g_trade_addr.sin_addr   = src_addr.sin_addr;
                    g_trade_addr_ready      = 1;
                }
                pthread_mutex_unlock(&g_trade_addr_mutex);
            }
        }
    }

    return NULL;
}

// ---------- Strategy thread ----------

typedef struct {
    CoreConfig *config;
} StrategyThreadArgs;

static void *strategy_thread(void *arg) {
    StrategyThreadArgs *sta = (StrategyThreadArgs *)arg;
    CoreConfig *cfg = sta->config;

    uint64_t current_second_start_ns = now_ns();
    int trades_in_current_second = 0;

    while (g_running) {
        uint64_t t_now = now_ns();

        if (t_now - current_second_start_ns >= 1000000000ULL) {
            current_second_start_ns = t_now;
            trades_in_current_second = 0;
        }

        PocketTraderState snapshot;
        memset(&snapshot, 0, sizeof(snapshot));

        if (pthread_mutex_lock(&g_shared->mutex) == 0) {
            snapshot = g_shared->state;
            pthread_mutex_unlock(&g_shared->mutex);
        }

        if (!g_running || snapshot.kill_switch || snapshot.circuit_tripped) {
            sched_yield();
            continue;
        }

        if (snapshot.strategy_mode == 0) {
            sched_yield();
            continue;
        }

        int exa_fresh = snapshot.exa.connected &&
                        (t_now - snapshot.exa.last_update_ns < STALE_THRESHOLD_NS);
        int exb_fresh = snapshot.exb.connected &&
                        (t_now - snapshot.exb.last_update_ns < STALE_THRESHOLD_NS);

        if (!exa_fresh || !exb_fresh) {
            sched_yield();
            continue;
        }

        double spread_exa_to_exb = snapshot.exb.bid - snapshot.exa.ask;
        double spread_exb_to_exa = snapshot.exa.bid - snapshot.exb.ask;

        int    do_trade    = 0;
        char   legA_exch[4] = {0};
        char   legB_exch[4] = {0};
        char   legA_side[5] = {0};
        char   legB_side[5] = {0};
        double legA_price  = 0.0;
        double legB_price  = 0.0;
        double used_spread = 0.0;

        if (spread_exa_to_exb >= snapshot.min_spread) {
            do_trade = 1;
            strcpy(legA_exch, "EXA");
            strcpy(legB_exch, "EXB");
            strcpy(legA_side, "BUY");
            strcpy(legB_side, "SELL");
            legA_price  = snapshot.exa.ask;
            legB_price  = snapshot.exb.bid;
            used_spread = spread_exa_to_exb;
        } else if (spread_exb_to_exa >= snapshot.min_spread) {
            do_trade = 1;
            strcpy(legA_exch, "EXB");
            strcpy(legB_exch, "EXA");
            strcpy(legA_side, "BUY");
            strcpy(legB_side, "SELL");
            legA_price  = snapshot.exb.ask;
            legB_price  = snapshot.exa.bid;
            used_spread = spread_exb_to_exa;
        }

        if (!do_trade) {
            if (pthread_mutex_lock(&g_shared->mutex) == 0) {
                g_shared->state.last_spread_exa_to_exb = spread_exa_to_exb;
                g_shared->state.last_spread_exb_to_exa = spread_exb_to_exa;
                pthread_mutex_unlock(&g_shared->mutex);
            }
            sched_yield();
            continue;
        }

        if (trades_in_current_second >= MAX_TRADES_PER_SECOND) {
            if (pthread_mutex_lock(&g_shared->mutex) == 0) {
                g_shared->state.rate_limited = 1;
                pthread_mutex_unlock(&g_shared->mutex);
            }
            sched_yield();
            continue;
        }

        struct sockaddr_in trade_addr_local;
        int trade_addr_ready_local;

        if (pthread_mutex_lock(&g_trade_addr_mutex) == 0) {
            trade_addr_ready_local = g_trade_addr_ready;
            trade_addr_local       = g_trade_addr;
            pthread_mutex_unlock(&g_trade_addr_mutex);
        } else {
            trade_addr_ready_local = 0;
        }

        if (!trade_addr_ready_local) {
            sched_yield();
            continue;
        }

        trade_addr_local.sin_port = htons(cfg->trade_port);

        uint64_t t_send = now_ns();
        uint64_t last_tick_ts =
            (snapshot.exa.last_update_ns > snapshot.exb.last_update_ns)
            ? snapshot.exa.last_update_ns
            : snapshot.exb.last_update_ns;
        uint64_t tick_to_trade_ns =
            (t_send > last_tick_ts) ? (t_send - last_tick_ts) : 0;

        char msg[256];
        double pnl = (legB_price - legA_price) * snapshot.trade_size;

        int len = snprintf(msg,
                           sizeof(msg),
                           "TRADE ARB1 %s %s %.6f %s %s %.6f %.6f %.6f %llu",
                           legA_exch,
                           legA_side,
                           legA_price,
                           legB_exch,
                           legB_side,
                           legB_price,
                           snapshot.trade_size,
                           used_spread,
                           (unsigned long long)t_send);
        if (len < 0 || len >= (int)sizeof(msg)) {
            fprintf(stderr, "TRADE message truncated\n");
            sched_yield();
            continue;
        }

        ssize_t sent = sendto(g_trade_sock,
                              msg,
                              (size_t)len,
                              0,
                              (struct sockaddr *)&trade_addr_local,
                              sizeof(trade_addr_local));
        if (sent < 0) {
            perror("sendto trade");
            sched_yield();
            continue;
        }

        trades_in_current_second++;

        if (pthread_mutex_lock(&g_shared->mutex) == 0) {
            PocketTraderState *st = &g_shared->state;
            st->last_spread_exa_to_exb = spread_exa_to_exb;
            st->last_spread_exb_to_exa = spread_exb_to_exa;
            st->last_trade_ts_ns       = t_send;
            st->last_tick_to_trade_ns  = tick_to_trade_ns;

            // ----- PnL & performance metrics -----
            st->last_trade_pnl = pnl;
            st->cumulative_pnl += pnl;
            st->trades_count   += 1;

            if (pnl >= 0.0) {
                st->gross_profit   += pnl;
                st->winning_trades += 1;
            } else {
                st->gross_loss     += -pnl;
                st->losing_trades  += 1;
            }

            // Equity curve & max drawdown
            if (st->trades_count == 1) {
                st->equity_high  = st->cumulative_pnl;
                st->max_drawdown = 0.0;
            } else {
                if (st->cumulative_pnl > st->equity_high) {
                    st->equity_high = st->cumulative_pnl;
                }
                double dd = st->cumulative_pnl - st->equity_high;
                if (dd < st->max_drawdown) {
                    st->max_drawdown = dd;
                }
            }

            // Circuit breaker
            if (st->cumulative_pnl < P_L_LIMIT) {
                st->circuit_tripped = 1;
                st->strategy_mode   = 0;
            }

            pthread_mutex_unlock(&g_shared->mutex);
        }

#if DEBUG_TICKS
        printf("[TRADE] %s/%s size=%.4f buy=%.2f sell=%.2f spread=%.4f pnl=%.4f\n",
               legA_exch,
               legB_exch,
               snapshot.trade_size,
               legA_price,
               legB_price,
               used_spread,
               pnl);
        fflush(stdout);
#endif

        // Log: t_now_ns, tick_to_trade_ns, exa_avg_interval_ns, exb_avg_interval_ns
        if (g_latency_log) {
            uint64_t t_log = now_ns();
            fprintf(g_latency_log,
                    "%llu,%llu,%llu,%llu\n",
                    (unsigned long long)t_log,
                    (unsigned long long)tick_to_trade_ns,
                    (unsigned long long)snapshot.avg_tick_latency_exa_ns,
                    (unsigned long long)snapshot.avg_tick_latency_exb_ns);
            fflush(g_latency_log);
        }

        sched_yield();
    }

    return NULL;
}

// ---------- Socket helpers ----------

static int create_bound_udp_socket(int port) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        exit(1);
    }

    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        exit(1);
    }

    return sock;
}

// ---------- Arg parsing ----------

static void parse_args(int argc, char **argv, CoreConfig *cfg) {
    cfg->exa_port   = DEFAULT_EXA_PORT;
    cfg->exb_port   = DEFAULT_EXB_PORT;
    cfg->trade_port = DEFAULT_TRADE_PORT;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--exa-port") == 0 && i + 1 < argc) {
            cfg->exa_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--exb-port") == 0 && i + 1 < argc) {
            cfg->exb_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--trade-port") == 0 && i + 1 < argc) {
            cfg->trade_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--exa-port N] [--exb-port N] [--trade-port N]\n",
                   argv[0]);
            exit(0);
        }
    }
}

// ---------- main ----------

int main(int argc, char **argv) {
    CoreConfig cfg;
    parse_args(argc, argv, &cfg);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    init_shared_memory();

    g_latency_log = fopen("latency_log.csv", "w");
    if (g_latency_log) {
        fprintf(g_latency_log,
                "t_now_ns,tick_to_trade_ns,exa_avg_tick_interval_ns,exb_avg_tick_interval_ns\n");
        fflush(g_latency_log);
    } else {
        perror("fopen latency_log.csv");
    }

    g_trade_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_trade_sock < 0) {
        perror("socket trade");
        exit(1);
    }

    int exa_sock = create_bound_udp_socket(cfg.exa_port);
    int exb_sock = create_bound_udp_socket(cfg.exb_port);

    pthread_t exa_thread;
    pthread_t exb_thread;
    pthread_t strat_thread;

    FeedThreadArgs exa_args  = { .sock = exa_sock, .is_exa = 1, .config = &cfg };
    FeedThreadArgs exb_args  = { .sock = exb_sock, .is_exa = 0, .config = &cfg };
    StrategyThreadArgs strat_args = { .config = &cfg };

    if (pthread_create(&exa_thread, NULL, feed_receiver_thread, &exa_args) != 0) {
        perror("pthread_create exa");
        exit(1);
    }
    if (pthread_create(&exb_thread, NULL, feed_receiver_thread, &exb_args) != 0) {
        perror("pthread_create exb");
        exit(1);
    }
    if (pthread_create(&strat_thread, NULL, strategy_thread, &strat_args) != 0) {
        perror("pthread_create strategy");
        exit(1);
    }

    while (g_running) {
        sleep(1);
    }

    pthread_join(exa_thread, NULL);
    pthread_join(exb_thread, NULL);
    pthread_join(strat_thread, NULL);

    close(exa_sock);
    close(exb_sock);
    close(g_trade_sock);

    if (g_shared) {
        munmap(g_shared, sizeof(*g_shared));
    }

    if (g_latency_log) {
        fclose(g_latency_log);
        g_latency_log = NULL;
    }

    return 0;
}
