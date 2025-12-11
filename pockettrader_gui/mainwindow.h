#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QTimer>
#include <QProgressBar>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QPushButton>
#include <QTableWidget>
#include <QStackedWidget>
#include <QList>
#include <cstdint>

extern "C" {
#include "pockettrader_state.h"   // from ../pockettrader_core_user_space
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void updateFromSharedMemory();
    void onMinSpreadChanged(double value);
    void onTradeSizeChanged(double value);
    void onModeChanged(int index);
    void onKillSwitchClicked();
    void onResetCircuitClicked();

private:
    void setupUi();
    void applyStyle();

    bool attachSharedMemory();
    void updateStatusBar(const PocketTraderState &st, std::uint64_t nowNs);
    void updateQuotePanel(const PocketTraderState &st);
    void updateLatencyPanel(const PocketTraderState &st);
    void updateControlPanel(const PocketTraderState &st);
    void updateTradeTape(const PocketTraderState &st);
    void updatePerformancePanel(const PocketTraderState &st);

    void autoScaleLabel(QLabel *label,
                        const QString &text,
                        int maxPt,
                        int minPt);
    void rescaleAllMajorLabels();

    // Shared memory
    PocketTraderShared *m_shared;
    bool                m_sharedAttached;

    // Timer
    QTimer *m_timer;

    // Top status bar
    QLabel *m_lblStatusExa;
    QLabel *m_lblStatusExb;
    QLabel *m_lblMode;
    QLabel *m_lblTrades;
    QLabel *m_lblPnl;

    // Quote panel
    QLabel *m_lblExaBid;
    QLabel *m_lblExaAsk;
    QLabel *m_lblExbBid;
    QLabel *m_lblExbAsk;

    QLabel *m_lblSpreadMain;
    QLabel *m_lblSpreadExaToExb;
    QLabel *m_lblSpreadExbToExa;
    QLabel *m_lblMinSpread;

    QPushButton *m_btnResetCircuit;

    // Latency panel
    QProgressBar *m_pbLatencyExa;
    QProgressBar *m_pbLatencyExb;
    QProgressBar *m_pbTickToTrade;

    QLabel *m_lblLatencyExaVal;
    QLabel *m_lblLatencyExbVal;
    QLabel *m_lblTickToTradeVal;
    QLabel *m_lblTickToTradeBest;
    QLabel *m_lblTickToTradeMedian;

    // Control panel
    QDoubleSpinBox *m_spinMinSpread;
    QDoubleSpinBox *m_spinTradeSize;
    QComboBox      *m_comboMode;
    QPushButton    *m_btnKillSwitch;

    // Tape
    QTableWidget *m_tblTrades;

    // Performance metrics labels
    QLabel *m_lblWinRate;
    QLabel *m_lblProfitFactor;
    QLabel *m_lblMaxDrawdown;

    // Navigation / pages
    QStackedWidget *m_stack;
    QPushButton *m_btnNavQuotes;
    QPushButton *m_btnNavControls;
    QPushButton *m_btnNavLatency;
    QPushButton *m_btnNavTrades;

    // Local trade stats
    std::uint32_t m_lastTradesCount;
    double        m_lastCumulativePnl;

    double        m_bestTickToTradeUs;
    QList<double> m_tickToTradeSamplesUs;
};

#endif // MAINWINDOW_H
