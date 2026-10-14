#pragma once
#include "trading/controller.h"
#include "profile/profile_controller.h"
#include "engine/grid.h"
#include "storage/store.h"
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

class QVBoxLayout;
class RebuildButton;

class Window : public QMainWindow {
    Q_OBJECT
public:
    Window(const QString& profile, const QString& instrument, QWidget* parent = nullptr,
        const QUrl& testEndpoint = {}, bool initialProduction = false);
    ~Window() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    grid::Engine engine;
    std::unique_ptr<Store> store;
    Client client;
    std::unique_ptr<TradeController> controller;
    std::unique_ptr<ProfileController> profiles;
    QTimer pumpTimer;
    QTimer readTimer;
    QElapsedTimer marketClock;
    QComboBox* instrumentBox = nullptr;
    QComboBox* environmentBox = nullptr;
    QLineEdit* idBox = nullptr;
    QLineEdit* secretBox = nullptr;
    QTableWidget* settings = nullptr;
    QTableWidget* cyclesTable = nullptr;
    QTableWidget* ordersTable = nullptr;
    QTableWidget* previewTable = nullptr;
    QPlainTextEdit* journal = nullptr;
    QLabel* connectionLabel = nullptr;
    QLabel* summaryLabel = nullptr;
    QLabel* statsLabel = nullptr;
    QLabel* latencyLabel = nullptr;
    QCheckBox* previewBox = nullptr;
    QPushButton* connectButton = nullptr;
    QPushButton* startButton = nullptr;
    QPushButton* pauseButton = nullptr;
    RebuildButton* applyButton = nullptr;
    QPushButton* cancelButton = nullptr;
    QPushButton* resetButton = nullptr;
    QPushButton* deleteProfileButton = nullptr;

    bool ready = false;
    bool syncing = false;
    bool busy = false;
    bool closing = false;
    bool closeAllowed = false;
    bool readBusy = false;
    bool storageFailed = false;
    int session = 0;
    int positionChecks = 0;
    quint64 positionRevision = 0;
    double netPosition = 0;
    double sessionPnl = 0;
    quint64 closedTrades = 0;
    bool statsReady = false;
    bool feesUnavailable = false;
    double sessionFees = 0;
    qint64 feesSince = 0;
    quint64 statsRevision = 0;
    QSet<QString> feeRequests;
    QHash<QString, grid::Int> recoveredFeeSkips;
    QString profilePath;
    QUrl testEndpoint;
    std::vector<std::string> reconcileLabels;
    size_t reconcileIndex = 0;
    QHash<QString, QJsonObject> reconcileSnapshots;
    std::vector<std::string> absentCreates;
    QJsonArray bufferedOrders;
    QJsonArray bufferedTrades;

    static QString text(const std::string& value);
    static double price(grid::Int value);
    static QString amount(grid::Int lots, const grid::Spec& spec);
    static QString role(grid::Role role);
    static QString positionDetails(const grid::Engine& engine, grid::Int actual);
    void setupUi();
    void setupConnectionControls(QVBoxLayout* layout);
    void setupTradingControls(QVBoxLayout* layout);
    void setupSettings(QVBoxLayout* layout);
    void setupGridControls(QVBoxLayout* layout);
    void setupSummary(QVBoxLayout* layout);
    void setupTablesAndTabs(QVBoxLayout* layout);
    void setupHistoryAndJournal(QVBoxLayout* layout);
    void setupFooter(QVBoxLayout* layout);
    void pauseGrid();
    void cancelBotOrders();
    void connectProfileController();
    void finishProfileCancellation();
    void finishProfileRecreation();
    void finishHistoryReset();
    grid::Config uiConfig() const;
    void setConfig(const grid::Config& config);
    void save();
    void log(const QString& text);
    void fail(const QString& text);
    void render();
    void renderControls();
    void renderSummary();
    void renderCycles();
    void renderOrders();
    void renderGridPreview();
    void updateLatency();
    void renderPreview(const grid::Preview& view);
    void pump();
    void submit(const grid::Action& action);
    void receiveSubmission(const RpcResult& r, int epoch, const grid::Action& a);
    void rejectSubmission(const grid::Action& a, const RpcResult& r);
    void acceptSubmission(const RpcResult& r);
    void connectExchange();
    void handleDisconnect();
    void handleSubscription(const QString& channel, const QJsonValue& payload);
    void initialize(const QJsonObject& auth);
    void loadInstrument(int epoch);
    void receiveInstrument(const RpcResult& response, int epoch);
    grid::Spec instrumentSpec(const QJsonObject& instrument) const;
    void bindAccount(int epoch);
    void disableCancelOnDisconnect(int epoch);
    void subscribeToUpdates(int epoch);
    void loadInitialTicker(int epoch);
    void synchronize();
    void receiveOpenOrders(const RpcResult& r, int epoch);
    void captureOpenOrder(const QJsonObject& order);
    void reconcileNext();
    void receiveReconciledOrder(const RpcResult& r, int epoch, const std::string& label);
    void resolveMissingOrder(const std::string& label);
    void reconcileHistory(const std::string& label, bool historical, int offset, int pass);
    void receiveOrderHistory(const RpcResult& r, int epoch, const std::string& label,
        bool historical, int offset, int pass);
    void continueHistorySearch(
        const std::string& label, bool historical, int offset, int pass, qsizetype count);
    void confirmAbsentCreate(const std::string& label, int pass);
    void confirmMissingCreate(const std::string& label);
    void readMissingCreate(int epoch, const std::string& label);
    void receiveMissingCreate(const RpcResult& r, int epoch, const std::string& label);
    void cancelProfileOrders(bool eraseProfile);
    void completeSync();
    void receiveSyncPosition(const RpcResult& r, int epoch, quint64 revision);
    void finishSyncPosition(const RpcResult& response, int epoch, quint64 revision);
    void releaseAbsentCreates(bool matched);
    void finishReconciliation(bool matched, grid::Int actual);
    void receiveOrder(const QJsonObject& order, bool resolve = false, bool persist = true,
        const std::string* label = nullptr);
    void receiveOrders(const QJsonArray& orders);
    void receiveForeignOrder(const QJsonObject& order);
    void receiveTrades(const QJsonArray& trades, grid::Int skipLots = -1);
    void refreshFees(const QString& orderId);
    void receiveFeeTrades(
        const RpcResult& r, const QString& id, int epoch, grid::Int expected, quint64 statsEpoch);
    void readMarket(const QJsonObject& ticker);
    void refreshAccount();
    void receiveAccountPosition(const RpcResult& response, int epoch, quint64 revision);
    void finishAccountRead(int epoch, quint64 revision, grid::Int actual, bool ok);
    void refreshTicker(int epoch);
    void startGrid(bool onlyChanged = false);
    void resetStats();
    void resetHistory();
};
