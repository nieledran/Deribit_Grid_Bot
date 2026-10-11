#pragma once
#include "controller.h"
#include "grid.h"
#include "store.h"
#include <QMainWindow>
#include <QLockFile>
#include <QTableWidget>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QJsonArray>
#include <QCheckBox>
#include <QHash>
#include <QSet>
#include <memory>

class Window : public QMainWindow {
    Q_OBJECT
public:
    Window(const QString& profile, const QString& instrument, QWidget* parent = nullptr, const QUrl& testEndpoint = {}, bool initialProduction = false);
    ~Window() override;
protected:
    void closeEvent(QCloseEvent* event) override;
private:
    grid::Engine engine;
    std::unique_ptr<Store> store;
    std::unique_ptr<QLockFile> profileLock, accountLock;
    Client client;
    std::unique_ptr<TradeController> controller;
    QTimer pumpTimer, readTimer;
    QElapsedTimer marketClock;
    QComboBox *instrumentBox = nullptr, *environmentBox = nullptr;
    QLineEdit *idBox = nullptr, *secretBox = nullptr;
    QTableWidget *settings = nullptr, *cyclesTable = nullptr, *ordersTable = nullptr, *previewTable = nullptr;
    QPlainTextEdit* journal = nullptr;
    QLabel *connectionLabel = nullptr, *summaryLabel = nullptr, *statsLabel = nullptr;
    QCheckBox* previewBox = nullptr;
    QPushButton *connectButton = nullptr, *startButton = nullptr, *pauseButton = nullptr, *applyButton = nullptr, *cancelButton = nullptr, *resetButton = nullptr;
    bool ready = false, syncing = false, busy = false, closing = false, closeAllowed = false;
    bool readBusy = false, storageFailed = false;
    int session = 0;
    int positionChecks = 0;
    quint64 positionRevision = 0;
    double netPosition = 0;
    double sessionPnl = 0;
    quint64 closedTrades = 0;
    bool statsReady = false, feesUnavailable = false;
    double sessionFees = 0;
    qint64 feesSince = 0;
    quint64 statsRevision = 0;
    QSet<QString> feeRequests;
    QString accountId;
    QUrl testEndpoint;
    std::vector<std::string> reconcileLabels;
    size_t reconcileIndex = 0;
    QHash<QString, QJsonObject> reconcileSnapshots;
    QJsonArray bufferedOrders;
    QJsonArray bufferedTrades;
    grid::Config uiConfig() const;
    void setConfig(const grid::Config& config);
    void save();
    void log(const QString& text);
    void fail(const QString& text);
    void render();
    void renderPreview(const grid::Preview& view);
    void pump();
    void submit(const grid::Action& action);
    void connectExchange();
    void initialize(const QJsonObject& auth);
    void synchronize();
    void reconcileNext();
    void completeSync();
    void receiveOrder(const QJsonObject& order, bool resolve = false, bool persist = true, const std::string* label = nullptr);
    void receiveOrders(const QJsonArray& orders);
    void receiveForeignOrder(const QJsonObject& order);
    void receiveTrades(const QJsonArray& trades);
    void refreshFees(const QString& orderId);
    void readMarket(const QJsonObject& ticker);
    void refreshAccount();
    void startGrid();
    void resetStats();
    void resetHistory();
};
