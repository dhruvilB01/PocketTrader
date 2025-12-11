#include "mainwindow.h"

#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QHeaderView>
#include <QTime>
#include <QResizeEvent>
#include <QFont>
#include <QFontMetrics>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#include <cstring>
#include <algorithm>
#include <math.h>

#ifndef STALE_THRESHOLD_NS
#define STALE_THRESHOLD_NS (500000000ULL)  // 0.5 seconds
#endif

static std::uint64_t now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (std::uint64_t)ts.tv_sec * 1000000000ULL +
           (std::uint64_t)ts.tv_nsec;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_shared          = nullptr;
    m_sharedAttached  = false;
    m_timer           = nullptr;

    m_lblStatusExa    = nullptr;
    m_lblStatusExb    = nullptr;
    m_lblMode         = nullptr;
    m_lblTrades       = nullptr;
    m_lblPnl          = nullptr;

    m_lblExaBid       = nullptr;
    m_lblExaAsk       = nullptr;
    m_lblExbBid       = nullptr;
    m_lblExbAsk       = nullptr;

    m_lblSpreadMain     = nullptr;
    m_lblSpreadExaToExb = nullptr;
    m_lblSpreadExbToExa = nullptr;
    m_lblMinSpread      = nullptr;

    m_btnResetCircuit = nullptr;

    m_pbLatencyExa    = nullptr;
    m_pbLatencyExb    = nullptr;
    m_pbTickToTrade   = nullptr;

    m_lblLatencyExaVal    = nullptr;
    m_lblLatencyExbVal    = nullptr;
    m_lblTickToTradeVal   = nullptr;
    m_lblTickToTradeBest  = nullptr;
    m_lblTickToTradeMedian = nullptr;

    m_spinMinSpread   = nullptr;
    m_spinTradeSize   = nullptr;
    m_comboMode       = nullptr;
    m_btnKillSwitch   = nullptr;

    m_tblTrades       = nullptr;

    m_lblWinRate      = nullptr;
    m_lblProfitFactor = nullptr;
    m_lblMaxDrawdown  = nullptr;

    m_stack           = nullptr;
    m_btnNavQuotes    = nullptr;
    m_btnNavControls  = nullptr;
    m_btnNavLatency   = nullptr;
    m_btnNavTrades    = nullptr;

    m_lastTradesCount    = 0;
    m_lastCumulativePnl  = 0.0;
    m_bestTickToTradeUs  = 0.0;

    // Global app font: 8 pt, light
    QFont appFont = qApp->font();
    appFont.setFamily("DejaVu Sans");
    appFont.setPointSize(8);
    appFont.setWeight(QFont::Light);
    qApp->setFont(appFont);

    setupUi();
    applyStyle();
    setFixedSize(480, 272);

    attachSharedMemory();

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout,
            this, &MainWindow::updateFromSharedMemory);
    m_timer->start(50);   // ~20 FPS polling
}

MainWindow::~MainWindow()
{
    if (m_sharedAttached && m_shared) {
        munmap(m_shared, sizeof(PocketTraderShared));
    }
}

// ----------------------------------------------------------------------
// UI SETUP
// ----------------------------------------------------------------------
void MainWindow::setupUi()
{
    setWindowTitle("PocketTrader Dashboard");
    setWindowFlag(Qt::FramelessWindowHint);

    QWidget *central = new QWidget(this);
    setCentralWidget(central);

    QVBoxLayout *root = new QVBoxLayout(central);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    // ------------------------------------------------------------------
    // 1) TOP STATUS BAR
    // ------------------------------------------------------------------
    QFrame *statusFrame = new QFrame(central);
    statusFrame->setObjectName("statusFrame");
    QHBoxLayout *statusLayout = new QHBoxLayout(statusFrame);
    statusLayout->setContentsMargins(6, 2, 6, 2);
    statusLayout->setSpacing(6);

    // Status labels font: 6 pt
    QFont fStatusBase;
    fStatusBase.setFamily("DejaVu Sans");
    fStatusBase.setPointSize(6);
    fStatusBase.setBold(false);

    m_lblStatusExa = new QLabel("EXA: DISCONNECTED");
    m_lblStatusExa->setFont(fStatusBase);

    m_lblStatusExb = new QLabel("EXB: DISCONNECTED");
    m_lblStatusExb->setFont(fStatusBase);

    m_lblMode = new QLabel("MODE: UNKNOWN");
    m_lblMode->setFont(fStatusBase);

    m_lblTrades = new QLabel("TRADES: 0");
    m_lblTrades->setFont(fStatusBase);

    // PnL: 8 pt
    QFont fPnl = fStatusBase;
    fPnl.setPointSize(8);
    m_lblPnl = new QLabel("PnL: 0.00");
    m_lblPnl->setFont(fPnl);

    // Small STOP button: 8 pt bold
    QPushButton *topKill = new QPushButton("STOP");
    topKill->setObjectName("topKillButton");
    topKill->setFixedHeight(18);
    QFont fTopKill = topKill->font();
    fTopKill.setPointSize(8);
    fTopKill.setBold(true);
    topKill->setFont(fTopKill);

    statusLayout->addWidget(m_lblStatusExa);
    statusLayout->addWidget(m_lblStatusExb);
    statusLayout->addWidget(m_lblMode);
    statusLayout->addStretch();
    statusLayout->addWidget(m_lblTrades);
    statusLayout->addWidget(m_lblPnl);
    statusLayout->addWidget(topKill);

    root->addWidget(statusFrame);

    // ------------------------------------------------------------------
    // 2) STACKED PAGES
    // ------------------------------------------------------------------
    m_stack = new QStackedWidget(central);
    root->addWidget(m_stack, 1);

    // =========================
    // PAGE 0: QUOTES
    // =========================
    QWidget *pageQuotes = new QWidget(m_stack);
    QVBoxLayout *pq = new QVBoxLayout(pageQuotes);
    pq->setContentsMargins(2, 2, 2, 2);
    pq->setSpacing(4);

    QFrame *quotesCard = new QFrame(pageQuotes);
    quotesCard->setObjectName("card");
    QVBoxLayout *qc = new QVBoxLayout(quotesCard);
    qc->setContentsMargins(6, 6, 6, 6);
    qc->setSpacing(4);

    QLabel *qtTitle = new QLabel("Dual-Exchange Quote Panel");
    qtTitle->setObjectName("sectionTitle");
    {
        QFont f = qtTitle->font();
        f.setPointSize(10);
        f.setBold(true);
        qtTitle->setFont(f);
    }
    qc->addWidget(qtTitle, 0, Qt::AlignHCenter);

    QGridLayout *qGrid = new QGridLayout;
    qGrid->setSpacing(4);

    // ---------- EXA card: BID big blue + ASK big yellow ----------
    QFrame *exaCard = new QFrame(quotesCard);
    exaCard->setObjectName("subCard");
    QVBoxLayout *exaLayout = new QVBoxLayout(exaCard);
    exaLayout->setContentsMargins(4, 4, 4, 4);
    exaLayout->setSpacing(2);

    QLabel *exaLabel = new QLabel("EXA");
    exaLabel->setObjectName("smallTitle");
    {
        QFont f = exaLabel->font();
        f.setPointSize(8);
        f.setBold(true);
        exaLabel->setFont(f);
    }
    exaLayout->addWidget(exaLabel, 0, Qt::AlignLeft);

    // EXA BID row
    QHBoxLayout *exaBidRow = new QHBoxLayout;
    QLabel *lblExaBidTag = new QLabel("BID");
    {
        QFont f = lblExaBidTag->font();
        f.setPointSize(8);
        f.setBold(true);
        lblExaBidTag->setFont(f);
    }
    m_lblExaBid = new QLabel("100.0000");
    m_lblExaBid->setObjectName("bigBid");
    {
        QFont f = m_lblExaBid->font();
        f.setPointSize(16);  // auto-scale 10–16
        f.setBold(true);
        m_lblExaBid->setFont(f);
    }
    exaBidRow->addWidget(lblExaBidTag);
    exaBidRow->addStretch();
    exaBidRow->addWidget(m_lblExaBid);
    exaLayout->addLayout(exaBidRow);

    // EXA ASK row (big)
    QHBoxLayout *exaAskRow = new QHBoxLayout;
    QLabel *lblExaAskTag = new QLabel("ASK");
    {
        QFont f = lblExaAskTag->font();
        f.setPointSize(8);
        f.setBold(true);
        lblExaAskTag->setFont(f);
    }
    m_lblExaAsk = new QLabel("100.0005");
    m_lblExaAsk->setObjectName("bigAsk");
    {
        QFont f = m_lblExaAsk->font();
        f.setPointSize(16);
        f.setBold(true);
        m_lblExaAsk->setFont(f);
    }
    exaAskRow->addWidget(lblExaAskTag);
    exaAskRow->addStretch();
    exaAskRow->addWidget(m_lblExaAsk);
    exaLayout->addLayout(exaAskRow);

    qGrid->addWidget(exaCard, 0, 0);

    // ---------- SPREAD card ----------
    QFrame *spreadCard = new QFrame(quotesCard);
    spreadCard->setObjectName("subCard");
    QVBoxLayout *spreadLayout = new QVBoxLayout(spreadCard);
    spreadLayout->setContentsMargins(4, 4, 4, 4);
    spreadLayout->setSpacing(2);

    QLabel *spreadLabel = new QLabel("SPREAD");
    spreadLabel->setObjectName("smallTitle");
    {
        QFont f = spreadLabel->font();
        f.setPointSize(8);
        f.setBold(true);
        spreadLabel->setFont(f);
    }
    spreadLayout->addWidget(spreadLabel, 0, Qt::AlignLeft);

    m_lblSpreadMain = new QLabel("0.0000");
    m_lblSpreadMain->setObjectName("bigNumberSpread");
    {
        QFont f = m_lblSpreadMain->font();
        f.setPointSize(16); // auto-scale 10–16
        f.setBold(true);
        m_lblSpreadMain->setFont(f);
    }
    spreadLayout->addWidget(m_lblSpreadMain, 0, Qt::AlignHCenter);

    m_lblSpreadExaToExb = new QLabel("EXA → EXB: 0.0000");
    m_lblSpreadExbToExa = new QLabel("EXB → EXA: 0.0000");
    {
        QFont f = m_lblSpreadExaToExb->font();
        f.setPointSize(8);
        f.setBold(false);
        m_lblSpreadExaToExb->setFont(f);
        m_lblSpreadExbToExa->setFont(f);
    }
    spreadLayout->addWidget(m_lblSpreadExaToExb);
    spreadLayout->addWidget(m_lblSpreadExbToExa);

    qGrid->addWidget(spreadCard, 0, 1);

    // ---------- EXB card: BID big blue + ASK big yellow ----------
    QFrame *exbCard = new QFrame(quotesCard);
    exbCard->setObjectName("subCard");
    QVBoxLayout *exbLayout = new QVBoxLayout(exbCard);
    exbLayout->setContentsMargins(4, 4, 4, 4);
    exbLayout->setSpacing(2);

    QLabel *exbLabel = new QLabel("EXB");
    exbLabel->setObjectName("smallTitle");
    {
        QFont f = exbLabel->font();
        f.setPointSize(8);
        f.setBold(true);
        exbLabel->setFont(f);
    }
    exbLayout->addWidget(exbLabel, 0, Qt::AlignLeft);

    // EXB BID row (big)
    QHBoxLayout *exbBidRow = new QHBoxLayout;
    QLabel *lblExbBidTag = new QLabel("BID");
    {
        QFont f = lblExbBidTag->font();
        f.setPointSize(8);
        f.setBold(true);
        lblExbBidTag->setFont(f);
    }
    m_lblExbBid = new QLabel("99.9995");
    m_lblExbBid->setObjectName("bigBid");
    {
        QFont f = m_lblExbBid->font();
        f.setPointSize(16);
        f.setBold(true);
        m_lblExbBid->setFont(f);
    }
    exbBidRow->addWidget(lblExbBidTag);
    exbBidRow->addStretch();
    exbBidRow->addWidget(m_lblExbBid);
    exbLayout->addLayout(exbBidRow);

    // EXB ASK row (big)
    QHBoxLayout *exbAskRow = new QHBoxLayout;
    QLabel *lblExbAskTag = new QLabel("ASK");
    {
        QFont f = lblExbAskTag->font();
        f.setPointSize(8);
        f.setBold(true);
        lblExbAskTag->setFont(f);
    }
    m_lblExbAsk = new QLabel("100.0000");
    m_lblExbAsk->setObjectName("bigAsk");
    {
        QFont f = m_lblExbAsk->font();
        f.setPointSize(16); // auto-scale 10–16
        f.setBold(true);
        m_lblExbAsk->setFont(f);
    }
    exbAskRow->addWidget(lblExbAskTag);
    exbAskRow->addStretch();
    exbAskRow->addWidget(m_lblExbAsk);
    exbLayout->addLayout(exbAskRow);

    qGrid->addWidget(exbCard, 0, 2);

    qc->addLayout(qGrid);

    // bottom row: just Reset Circuit aligned right (SXRAAD / Mr Spread removed)
    QHBoxLayout *resetRow = new QHBoxLayout;
    m_btnResetCircuit = new QPushButton("Reset Circuit");
    m_btnResetCircuit->setObjectName("resetButton");
    {
        QFont f = m_btnResetCircuit->font();
        f.setPointSize(8);
        f.setBold(false);
        m_btnResetCircuit->setFont(f);
    }
    resetRow->addStretch();
    resetRow->addWidget(m_btnResetCircuit);
    qc->addLayout(resetRow);

    // Min spread label: 8 pt
    m_lblMinSpread = new QLabel("Min spread: 0.0000");
    {
        QFont f = m_lblMinSpread->font();
        f.setPointSize(8);
        f.setBold(false);
        m_lblMinSpread->setFont(f);
    }
    qc->addWidget(m_lblMinSpread);

    pq->addWidget(quotesCard, 1);
    m_stack->addWidget(pageQuotes); // index 0

    // =========================
    // PAGE 1: CONTROL
    // =========================
    QWidget *pageControl = new QWidget(m_stack);
    QVBoxLayout *pcRoot = new QVBoxLayout(pageControl);
    pcRoot->setContentsMargins(2, 2, 2, 2);
    pcRoot->setSpacing(4);

    QFrame *ctrlCard = new QFrame(pageControl);
    ctrlCard->setObjectName("card");
    QVBoxLayout *ctrlLayout = new QVBoxLayout(ctrlCard);
    ctrlLayout->setContentsMargins(6, 6, 6, 6);
    ctrlLayout->setSpacing(6);

    QGridLayout *ctrlGrid = new QGridLayout;
    ctrlGrid->setSpacing(4);

    QFont fCtrlLabel;
    fCtrlLabel.setFamily("DejaVu Sans");
    fCtrlLabel.setPointSize(8);
    fCtrlLabel.setBold(false);

    QLabel *lblMin = new QLabel("Min spread:");
    lblMin->setFont(fCtrlLabel);
    m_spinMinSpread = new QDoubleSpinBox;
    m_spinMinSpread->setDecimals(4);
    m_spinMinSpread->setRange(0.0, 1000.0);
    m_spinMinSpread->setSingleStep(0.0001);
    {
        QFont f = m_spinMinSpread->font();
        f.setPointSize(8);
        f.setBold(false);
        m_spinMinSpread->setFont(f);
    }

    QLabel *lblMode = new QLabel("Mode:");
    lblMode->setFont(fCtrlLabel);
    m_comboMode = new QComboBox;
    m_comboMode->addItem("OFF", 0);
    m_comboMode->addItem("MONITOR", 1);
    m_comboMode->addItem("PAPER", 2);
    {
        QFont f = m_comboMode->font();
        f.setPointSize(8);
        f.setBold(false);
        m_comboMode->setFont(f);
    }

    QLabel *lblSize = new QLabel("Trade size:");
    lblSize->setFont(fCtrlLabel);
    m_spinTradeSize = new QDoubleSpinBox;
    m_spinTradeSize->setDecimals(4);
    m_spinTradeSize->setRange(0.0, 100000.0);
    m_spinTradeSize->setSingleStep(0.001);
    {
        QFont f = m_spinTradeSize->font();
        f.setPointSize(8);
        f.setBold(false);
        m_spinTradeSize->setFont(f);
    }

    ctrlGrid->addWidget(lblMin,          0, 0);
    ctrlGrid->addWidget(m_spinMinSpread, 0, 1);
    ctrlGrid->addWidget(lblMode,         0, 2);
    ctrlGrid->addWidget(m_comboMode,     0, 3);

    ctrlGrid->addWidget(lblSize,         1, 0);
    ctrlGrid->addWidget(m_spinTradeSize, 1, 1);

    ctrlLayout->addLayout(ctrlGrid);

    // KILL SWITCH button: 14 pt bold
    m_btnKillSwitch = new QPushButton("KILL SWITCH");
    m_btnKillSwitch->setObjectName("killButton");
    m_btnKillSwitch->setMinimumHeight(50);
    {
        QFont f = m_btnKillSwitch->font();
        f.setPointSize(14);
        f.setBold(true);
        m_btnKillSwitch->setFont(f);
    }
    ctrlLayout->addWidget(m_btnKillSwitch, 0, Qt::AlignHCenter);

    pcRoot->addWidget(ctrlCard, 1);
    m_stack->addWidget(pageControl); // index 1

    // =========================
    // PAGE 2: LATENCY
    // =========================
    QWidget *pageLatency = new QWidget(m_stack);
    QVBoxLayout *pl = new QVBoxLayout(pageLatency);
    pl->setContentsMargins(2, 2, 2, 2);
    pl->setSpacing(4);

    QFrame *latCard = new QFrame(pageLatency);
    latCard->setObjectName("card");
    QVBoxLayout *latLayout = new QVBoxLayout(latCard);
    latLayout->setContentsMargins(6, 6, 6, 6);
    latLayout->setSpacing(6);

    QLabel *latTitle = new QLabel("Latency");
    latTitle->setObjectName("sectionTitle");
    {
        QFont f = latTitle->font();
        f.setPointSize(10);
        f.setBold(true);
        latTitle->setFont(f);
    }
    latLayout->addWidget(latTitle, 0, Qt::AlignHCenter);

    QLabel *feedLabel = new QLabel("Host → BBB feed latency:");
    feedLabel->setObjectName("smallTitle");
    {
        QFont f = feedLabel->font();
        f.setPointSize(8);
        f.setBold(true);
        feedLabel->setFont(f);
    }
    latLayout->addWidget(feedLabel);

    m_pbLatencyExa = new QProgressBar(latCard);
    m_pbLatencyExb = new QProgressBar(latCard);
    m_pbLatencyExa->setRange(0, 10000);
    m_pbLatencyExb->setRange(0, 10000);
    m_pbLatencyExa->setTextVisible(false);
    m_pbLatencyExb->setTextVisible(false);
    m_pbLatencyExa->setFixedHeight(12);
    m_pbLatencyExb->setFixedHeight(12);

    m_lblLatencyExaVal = new QLabel("0.00 ms");
    m_lblLatencyExbVal = new QLabel("0.00 ms");
    {
        QFont f = m_lblLatencyExaVal->font();
        f.setPointSize(8);
        f.setBold(false);
        m_lblLatencyExaVal->setFont(f);
        m_lblLatencyExbVal->setFont(f);
    }

    QHBoxLayout *exaRow = new QHBoxLayout;
    exaRow->addWidget(m_pbLatencyExa, 1);
    exaRow->addWidget(m_lblLatencyExaVal);

    QHBoxLayout *exbRow = new QHBoxLayout;
    exbRow->addWidget(m_pbLatencyExb, 1);
    exbRow->addWidget(m_lblLatencyExbVal);

    latLayout->addLayout(exaRow);
    latLayout->addLayout(exbRow);

    QLabel *ttLabel = new QLabel("Tick → trade latency:");
    ttLabel->setObjectName("smallTitle");
    {
        QFont f = ttLabel->font();
        f.setPointSize(8);
        f.setBold(true);
        ttLabel->setFont(f);
    }
    latLayout->addWidget(ttLabel);

    m_pbTickToTrade = new QProgressBar(latCard);
    m_pbTickToTrade->setRange(0, 2000);
    m_pbTickToTrade->setTextVisible(false);
    m_pbTickToTrade->setFixedHeight(12);
    m_lblTickToTradeVal = new QLabel("0 µs");
    {
        QFont f = m_lblTickToTradeVal->font();
        f.setPointSize(8);
        f.setBold(false);
        m_lblTickToTradeVal->setFont(f);
    }

    QHBoxLayout *ttRow = new QHBoxLayout;
    ttRow->addWidget(m_pbTickToTrade, 1);
    ttRow->addWidget(m_lblTickToTradeVal);

    latLayout->addLayout(ttRow);

    m_lblTickToTradeBest   = new QLabel("Best: - µs");
    m_lblTickToTradeMedian = new QLabel("Median: - µs");
    {
        QFont f = m_lblTickToTradeBest->font();
        f.setPointSize(8);
        f.setBold(false);
        m_lblTickToTradeBest->setFont(f);
        m_lblTickToTradeMedian->setFont(f);
    }
    latLayout->addWidget(m_lblTickToTradeBest);
    latLayout->addWidget(m_lblTickToTradeMedian);

    pl->addWidget(latCard, 1);
    m_stack->addWidget(pageLatency); // index 2

    // =========================
    // PAGE 3: TAPE + PERFORMANCE
    // =========================
    QWidget *pageTape = new QWidget(m_stack);
    QVBoxLayout *pt = new QVBoxLayout(pageTape);
    pt->setContentsMargins(2, 2, 2, 2);
    pt->setSpacing(4);

    QFrame *tapeCard = new QFrame(pageTape);
    tapeCard->setObjectName("card");
    QVBoxLayout *tapeLayout = new QVBoxLayout(tapeCard);
    tapeLayout->setContentsMargins(6, 6, 6, 6);
    tapeLayout->setSpacing(4);

    QLabel *tapeTitle = new QLabel("Trade Tape");
    tapeTitle->setObjectName("sectionTitle");
    {
        QFont f = tapeTitle->font();
        f.setPointSize(10);
        f.setBold(true);
        tapeTitle->setFont(f);
    }
    tapeLayout->addWidget(tapeTitle, 0, Qt::AlignHCenter);

    // Performance metrics row
    QHBoxLayout *perfRow = new QHBoxLayout;
    perfRow->setSpacing(6);

    m_lblWinRate      = new QLabel("Win rate: 0.0 %");
    m_lblProfitFactor = new QLabel("Profit factor: 0.00");
    m_lblMaxDrawdown  = new QLabel("Max drawdown: 0.00");

    {
        QFont f = m_lblWinRate->font();
        f.setPointSize(8);
        f.setBold(false);
        m_lblWinRate->setFont(f);
        m_lblProfitFactor->setFont(f);
        m_lblMaxDrawdown->setFont(f);
    }

    perfRow->addWidget(m_lblWinRate);
    perfRow->addWidget(m_lblProfitFactor);
    perfRow->addWidget(m_lblMaxDrawdown);
    perfRow->addStretch();

    tapeLayout->addLayout(perfRow);

    // Trade table
    m_tblTrades = new QTableWidget(0, 5, tapeCard);
    QStringList headers;
    headers << "Time" << "Dir" << "Spread" << "Size" << "PnL";
    m_tblTrades->setHorizontalHeaderLabels(headers);
    m_tblTrades->horizontalHeader()->setStretchLastSection(true);
    m_tblTrades->verticalHeader()->setVisible(false);
    m_tblTrades->setSelectionMode(QAbstractItemView::NoSelection);
    m_tblTrades->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tblTrades->setAlternatingRowColors(true);
    m_tblTrades->setShowGrid(false);
    m_tblTrades->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft);

    QFont fHeader = m_tblTrades->horizontalHeader()->font();
    fHeader.setPointSize(8);
    fHeader.setBold(true);
    m_tblTrades->horizontalHeader()->setFont(fHeader);

    QFont fRow = m_tblTrades->font();
    fRow.setPointSize(7);
    fRow.setBold(false);
    m_tblTrades->setFont(fRow);

    tapeLayout->addWidget(m_tblTrades, 1);
    pt->addWidget(tapeCard, 1);
    m_stack->addWidget(pageTape); // index 3

    // ------------------------------------------------------------------
    // BOTTOM NAV BAR
    // ------------------------------------------------------------------
    QFrame *navFrame = new QFrame(central);
    navFrame->setObjectName("navFrame");
    QHBoxLayout *nav = new QHBoxLayout(navFrame);
    nav->setContentsMargins(2, 2, 2, 2);
    nav->setSpacing(4);

    QFont fNav;
    fNav.setFamily("DejaVu Sans");
    fNav.setPointSize(8);
    fNav.setBold(true);

    m_btnNavQuotes   = new QPushButton("QUOTES");
    m_btnNavControls = new QPushButton("CONTROL");
    m_btnNavLatency  = new QPushButton("LATENCY");
    m_btnNavTrades   = new QPushButton("TAPE");

    m_btnNavQuotes->setFont(fNav);
    m_btnNavControls->setFont(fNav);
    m_btnNavLatency->setFont(fNav);
    m_btnNavTrades->setFont(fNav);

    nav->addWidget(m_btnNavQuotes);
    nav->addWidget(m_btnNavControls);
    nav->addWidget(m_btnNavLatency);
    nav->addWidget(m_btnNavTrades);

    root->addWidget(navFrame);

    // navigation
    connect(m_btnNavQuotes, &QPushButton::clicked,
            this, [this]{ m_stack->setCurrentIndex(0); });
    connect(m_btnNavControls, &QPushButton::clicked,
            this, [this]{ m_stack->setCurrentIndex(1); });
    connect(m_btnNavLatency, &QPushButton::clicked,
            this, [this]{ m_stack->setCurrentIndex(2); });
    connect(m_btnNavTrades, &QPushButton::clicked,
            this, [this]{ m_stack->setCurrentIndex(3); });

    // control → slots
    connect(m_spinMinSpread, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::onMinSpreadChanged);
    connect(m_spinTradeSize, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::onTradeSizeChanged);
    connect(m_comboMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onModeChanged);

    connect(m_btnKillSwitch, &QPushButton::clicked,
            this, &MainWindow::onKillSwitchClicked);
    connect(topKill, &QPushButton::clicked,
            this, &MainWindow::onKillSwitchClicked);

    connect(m_btnResetCircuit, &QPushButton::clicked,
            this, &MainWindow::onResetCircuitClicked);

    m_stack->setCurrentIndex(0);

    // initial scaling
    rescaleAllMajorLabels();
}

// ----------------------------------------------------------------------
// AUTO-SCALING
// ----------------------------------------------------------------------
void MainWindow::autoScaleLabel(QLabel *label,
                                const QString &text,
                                int maxPt,
                                int minPt)
{
    if (!label)
        return;

    int w = label->width();
    if (w <= 0) {
        label->setText(text);
        return;
    }

    QFont base = label->font();

    for (int pt = maxPt; pt >= minPt; --pt) {
        QFont f = base;
        f.setPointSize(pt);
        QFontMetrics fm(f);
        int textWidth = fm.horizontalAdvance(text);
        if (textWidth <= w - 4) {
            label->setFont(f);
            label->setText(text);
            return;
        }
    }

    QFont f = base;
    f.setPointSize(minPt);
    label->setFont(f);
    label->setText(text);
}

void MainWindow::rescaleAllMajorLabels()
{
    // Big numbers: 10–16 pt
    if (m_lblExaBid)
        autoScaleLabel(m_lblExaBid, m_lblExaBid->text(), 16, 10);
    if (m_lblExaAsk)
        autoScaleLabel(m_lblExaAsk, m_lblExaAsk->text(), 16, 10);
    if (m_lblExbBid)
        autoScaleLabel(m_lblExbBid, m_lblExbBid->text(), 16, 10);
    if (m_lblExbAsk)
        autoScaleLabel(m_lblExbAsk, m_lblExbAsk->text(), 16, 10);
    if (m_lblSpreadMain)
        autoScaleLabel(m_lblSpreadMain, m_lblSpreadMain->text(), 16, 10);

    // Status bar: 6–8 pt
    if (m_lblStatusExa)
        autoScaleLabel(m_lblStatusExa, m_lblStatusExa->text(), 8, 6);
    if (m_lblStatusExb)
        autoScaleLabel(m_lblStatusExb, m_lblStatusExb->text(), 8, 6);
    if (m_lblMode)
        autoScaleLabel(m_lblMode, m_lblMode->text(), 8, 6);
    if (m_lblTrades)
        autoScaleLabel(m_lblTrades, m_lblTrades->text(), 8, 6);
    if (m_lblPnl)
        autoScaleLabel(m_lblPnl, m_lblPnl->text(), 8, 6);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    rescaleAllMajorLabels();
}

// ----------------------------------------------------------------------
// STYLESHEET
// ----------------------------------------------------------------------
void MainWindow::applyStyle()
{
    QString style = R"(
        QMainWindow {
            background-color: #10141a;
            color: #f0f0f0;
        }
        QLabel {
            color: #f0f0f0;
        }
        QLabel#sectionTitle {
            font-weight: bold;
        }
        QLabel#smallTitle {
            font-weight: bold;
            letter-spacing: 1px;
            color: #a0a6b4;
        }
        QLabel#bigBid {
            font-weight: 600;
            color: #00c0ff;     /* blue for bids */
        }
        QLabel#bigAsk {
            font-weight: 600;
            color: #ffd45a;     /* yellow for asks */
        }
        QLabel#bigNumberSpread {
            font-weight: 600;
            color: #1dd1a1;
        }
        QFrame#card {
            background-color: #181d24;
            border-radius: 10px;
            border: 1px solid #262c36;
        }
        QFrame#subCard {
            background-color: #151a20;
            border-radius: 8px;
            border: 1px solid #262c36;
        }
        QFrame#statusFrame {
            background-color: #151a20;
            border-radius: 6px;
            border: 1px solid #262c36;
        }
        QPushButton#killButton {
            background-color: #ff5c5c;
            border-radius: 10px;
            border: none;
            color: white;
            font-weight: bold;
            padding: 4px 8px;
        }
        QPushButton#killButton:pressed {
            background-color: #e04848;
        }
        QPushButton#resetButton {
            background-color: transparent;
            border-radius: 6px;
            border: 1px solid #ffb84d;
            color: #ffb84d;
            padding: 2px 8px;
        }
        QProgressBar {
            background-color: #151a20;
            border-radius: 4px;
            border: 1px solid #262c36;
            text-align: right;
            padding-right: 4px;
            color: #f0f0f0;
        }
        QProgressBar::chunk {
            background-color: #00d1b2;
            border-radius: 4px;
        }
        QTableWidget {
            background-color: #151a20;
            border-radius: 6px;
            border: 1px solid #262c36;
            gridline-color: #262c36;
            alternate-background-color: #181e26;
        }
        QHeaderView::section {
            background-color: #181d24;
            color: #a0a6b4;
            border: none;
            border-bottom: 1px solid #262c36;
            padding: 2px 6px;
        }
        QDoubleSpinBox, QComboBox {
            background-color: #151a20;
            border-radius: 4px;
            border: 1px solid #262c36;
            padding: 2px 6px;
        }
    )";
    setStyleSheet(style);
}

// ----------------------------------------------------------------------
// SHARED MEMORY
// ----------------------------------------------------------------------
bool MainWindow::attachSharedMemory()
{
    if (m_sharedAttached)
        return true;

    int fd = shm_open(POCKETTRADER_SHM_NAME, O_RDWR, 0666);
    if (fd < 0) {
        return false;  // core not up yet
    }

    void *addr = mmap(nullptr, sizeof(PocketTraderShared),
                      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);

    if (addr == MAP_FAILED) {
        return false;
    }

    m_shared = static_cast<PocketTraderShared*>(addr);
    m_sharedAttached = true;
    return true;
}

// ----------------------------------------------------------------------
// PERIODIC UPDATE
// ----------------------------------------------------------------------
void MainWindow::updateFromSharedMemory()
{
    if (!m_sharedAttached) {
        if (!attachSharedMemory())
            return;
    }

    PocketTraderState st;
    std::memset(&st, 0, sizeof(st));

    if (pthread_mutex_lock(&m_shared->mutex) == 0) {
        st = m_shared->state;  // copy only, no UI work under the lock
        pthread_mutex_unlock(&m_shared->mutex);
    } else {
        return;
    }

    std::uint64_t now = now_ns();

    updateStatusBar(st, now);
    updateQuotePanel(st);
    updateLatencyPanel(st);
    updateControlPanel(st);
    updateTradeTape(st);
    updatePerformancePanel(st);

    rescaleAllMajorLabels();
}

// ----------------------------------------------------------------------
// UI UPDATE HELPERS
// ----------------------------------------------------------------------
void MainWindow::updateStatusBar(const PocketTraderState &st,
                                 std::uint64_t nowNs)
{
    auto fmtStatus = [nowNs](const ExchangeQuote &q) -> QString {
        if (!q.connected)
            return "DISCONNECTED";
        std::uint64_t ageNs = nowNs - q.last_update_ns;
        if (ageNs > STALE_THRESHOLD_NS)
            return "STALE";
        return "CONNECTED";
    };

    QString exaStatus = fmtStatus(st.exa);
    QString exbStatus = fmtStatus(st.exb);

    m_lblStatusExa->setText(QString("EXA: %1").arg(exaStatus));
    m_lblStatusExb->setText(QString("EXB: %1").arg(exbStatus));

    QString modeStr = "UNKNOWN";
    switch (st.strategy_mode) {
    case 0: modeStr = "OFF";      break;
    case 1: modeStr = "MONITOR";  break;
    case 2: modeStr = "PAPER";    break;
    }

    if (st.circuit_tripped) {
        modeStr += " (CIRCUIT)";
    }

    m_lblMode->setText(QString("MODE: %1").arg(modeStr));
    m_lblTrades->setText(QString("TRADES: %1").arg(st.trades_count));
    m_lblPnl->setText(QString("PnL: %1").arg(st.cumulative_pnl, 0, 'f', 2));
}

void MainWindow::updateQuotePanel(const PocketTraderState &st)
{
    // EXA / EXB bid/ask
    m_lblExaBid->setText(QString::number(st.exa.bid, 'f', 4));
    m_lblExaAsk->setText(QString::number(st.exa.ask, 'f', 4));
    m_lblExbBid->setText(QString::number(st.exb.bid, 'f', 4));
    m_lblExbAsk->setText(QString::number(st.exb.ask, 'f', 4));

    // spreads from state
    double s1 = st.last_spread_exa_to_exb;
    double s2 = st.last_spread_exb_to_exa;
    double mainSpread = std::max(s1, s2);

    m_lblSpreadMain->setText(QString::number(mainSpread, 'f', 4));
    m_lblSpreadExaToExb->setText(
                QString("EXA → EXB: %1").arg(s1, 0, 'f', 4));
    m_lblSpreadExbToExa->setText(
                QString("EXB → EXA: %1").arg(s2, 0, 'f', 4));

    m_lblMinSpread->setText(
                QString("Min spread: %1").arg(st.min_spread, 0, 'f', 4));
}

void MainWindow::updateLatencyPanel(const PocketTraderState &st)
{
    double exaMs = st.avg_tick_latency_exa_ns / 1e6;
    double exbMs = st.avg_tick_latency_exb_ns / 1e6;

    int exaUs = (int)std::min(9999.0,
                              st.avg_tick_latency_exa_ns / 1000.0);
    int exbUs = (int)std::min(9999.0,
                              st.avg_tick_latency_exb_ns / 1000.0);

    m_pbLatencyExa->setValue(exaUs);
    m_pbLatencyExb->setValue(exbUs);

    m_lblLatencyExaVal->setText(QString::number(exaMs, 'f', 2) + " ms");
    m_lblLatencyExbVal->setText(QString::number(exbMs, 'f', 2) + " ms");

    double ttUs = st.last_tick_to_trade_ns / 1000.0;
    int ttUsInt = (int)std::min(2000.0, ttUs);

    m_pbTickToTrade->setValue(ttUsInt);
    m_lblTickToTradeVal->setText(QString::number(ttUs, 'f', 0) + " µs");

    if (ttUs > 0.0) {
        if (m_bestTickToTradeUs == 0.0 || ttUs < m_bestTickToTradeUs)
            m_bestTickToTradeUs = ttUs;

        m_tickToTradeSamplesUs.append(ttUs);
        if (m_tickToTradeSamplesUs.size() > 200)
            m_tickToTradeSamplesUs.removeFirst();

        m_lblTickToTradeBest->setText(
                    QString("Best so far: %1 µs")
                    .arg(m_bestTickToTradeUs, 0, 'f', 0));

        QList<double> copy = m_tickToTradeSamplesUs;
        std::sort(copy.begin(), copy.end());
        double median = copy[copy.size()/2];
        m_lblTickToTradeMedian->setText(
                    QString("Median: %1 µs").arg(median, 0, 'f', 0));
    }
}

void MainWindow::updateControlPanel(const PocketTraderState &st)
{
    bool old1 = m_spinMinSpread->blockSignals(true);
    bool old2 = m_spinTradeSize->blockSignals(true);
    bool old3 = m_comboMode->blockSignals(true);

    m_spinMinSpread->setValue(st.min_spread);
    m_spinTradeSize->setValue(st.trade_size);

    int modeIdx = 0;
    if (st.strategy_mode == 1) modeIdx = 1;
    else if (st.strategy_mode == 2) modeIdx = 2;
    m_comboMode->setCurrentIndex(modeIdx);

    m_spinMinSpread->blockSignals(old1);
    m_spinTradeSize->blockSignals(old2);
    m_comboMode->blockSignals(old3);
}

void MainWindow::updateTradeTape(const PocketTraderState &st)
{
    if (!m_tblTrades)
        return;

    if (st.trades_count <= m_lastTradesCount)
        return;

    // Only log one row per update: last trade
    QString dir;
    double usedSpread;
    if (st.last_spread_exa_to_exb >= st.last_spread_exb_to_exa) {
        dir = "EXA→EXB";
        usedSpread = st.last_spread_exa_to_exb;
    } else {
        dir = "EXB→EXA";
        usedSpread = st.last_spread_exb_to_exa;
    }

    int row = m_tblTrades->rowCount();
    m_tblTrades->insertRow(row);

    m_tblTrades->setItem(row, 0,
        new QTableWidgetItem(QTime::currentTime().toString("HH:mm:ss")));
    m_tblTrades->setItem(row, 1,
        new QTableWidgetItem(dir));
    m_tblTrades->setItem(row, 2,
        new QTableWidgetItem(QString::number(usedSpread, 'f', 4)));
    m_tblTrades->setItem(row, 3,
        new QTableWidgetItem(QString::number(st.trade_size, 'f', 4)));
    m_tblTrades->setItem(row, 4,
        new QTableWidgetItem(QString::number(st.last_trade_pnl, 'f', 4)));

    while (m_tblTrades->rowCount() > 50) {
        m_tblTrades->removeRow(0);
    }

    m_lastTradesCount   = st.trades_count;
    m_lastCumulativePnl = st.cumulative_pnl;
}

void MainWindow::updatePerformancePanel(const PocketTraderState &st)
{
    if (!m_lblWinRate || !m_lblProfitFactor || !m_lblMaxDrawdown)
        return;

    double winRate = 0.0;
    if (st.trades_count > 0) {
        winRate = 100.0 * (double)st.winning_trades /
                  (double)st.trades_count;
    }

    QString pfText;
    if (st.gross_loss > 0.0) {
        double profitFactor = st.gross_profit / st.gross_loss;
        pfText = QString("Profit factor: %1").arg(profitFactor, 0, 'f', 2);
    } else if (st.gross_profit > 0.0) {
        pfText = QString("Profit factor: N/A");
    } else {
        pfText = QString("Profit factor: 0.00");
    }

    double maxDDAbs = -st.max_drawdown;
    if (maxDDAbs < 0.0) maxDDAbs = 0.0;

    m_lblWinRate->setText(
        QString("Win rate: %1 %").arg(winRate, 0, 'f', 1));
    m_lblProfitFactor->setText(pfText);
    m_lblMaxDrawdown->setText(
        QString("Max drawdown: %1").arg(maxDDAbs, 0, 'f', 2));
}

// ----------------------------------------------------------------------
// SLOTS: write back into shared memory
// ----------------------------------------------------------------------
void MainWindow::onMinSpreadChanged(double value)
{
    if (!m_sharedAttached) return;
    if (pthread_mutex_lock(&m_shared->mutex) == 0) {
        m_shared->state.min_spread = value;
        pthread_mutex_unlock(&m_shared->mutex);
    }
}

void MainWindow::onTradeSizeChanged(double value)
{
    if (!m_sharedAttached) return;
    if (pthread_mutex_lock(&m_shared->mutex) == 0) {
        m_shared->state.trade_size = value;
        pthread_mutex_unlock(&m_shared->mutex);
    }
}

void MainWindow::onModeChanged(int index)
{
    if (!m_sharedAttached) return;
    int mode = m_comboMode->itemData(index).toInt();
    if (pthread_mutex_lock(&m_shared->mutex) == 0) {
        m_shared->state.strategy_mode = mode;
        pthread_mutex_unlock(&m_shared->mutex);
    }
}

void MainWindow::onKillSwitchClicked()
{
    if (!m_sharedAttached) return;
    if (pthread_mutex_lock(&m_shared->mutex) == 0) {
        m_shared->state.kill_switch = 1;
        pthread_mutex_unlock(&m_shared->mutex);
    }
}

void MainWindow::onResetCircuitClicked()
{
    if (!m_sharedAttached) return;
    if (pthread_mutex_lock(&m_shared->mutex) == 0) {
        m_shared->state.circuit_tripped = 0;
        m_shared->state.kill_switch = 0;
        if (m_shared->state.strategy_mode == 0)
            m_shared->state.strategy_mode = 2; // resume PAPER
        pthread_mutex_unlock(&m_shared->mutex);
    }
}
