#include "window.h"
#include "api_numbers.h"
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QSpinBox>
#include <QStylePainter>
#include <QStyleOptionButton>
#include <QTabWidget>
#include <QVBoxLayout>
#include <cmath>
#include <stdexcept>

using namespace grid;
using namespace deribit;
namespace {
QString money(Int value) { return QString::number(static_cast<double>(value) / Dollars, 'f', 2); }
QString state(Status s) {
    switch (s) { case Status::Pending: return "Pending"; case Status::Open: return "Open"; case Status::Uncertain: return "Uncertain";
        case Status::Terminal: return "Closed"; case Status::Rejected: return "Rejected"; } return "Invalid";
}
void cell(QTableWidget* table, int row, int col, const QString& value) {
    auto* item = table->item(row, col);
    if (!item) table->setItem(row, col, new QTableWidgetItem(value));
    else if (item->text() != value) item->setText(value);
}
void setupTable(QTableWidget* table, const QStringList& headers) {
    table->setColumnCount(headers.size()); table->setHorizontalHeaderLabels(headers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->setAlternatingRowColors(true);
}
class RebuildButton : public QPushButton {
    QTimer stripes;
    QPalette normal;
    int offset = 0;
public:
    explicit RebuildButton(const QString& label) : QPushButton(label), normal(palette()) {
        stripes.setInterval(100);
        connect(&stripes, &QTimer::timeout, this, [this] { offset = (offset + 1) % 24; update(); });
    }
    void setTransition(bool active) {
        if (active == stripes.isActive()) return;
        if (active) {
            auto blue = normal; blue.setColor(QPalette::ButtonText, QColor("#1d4ed8"));
            blue.setColor(QPalette::Button, QColor("#dbeafe")); setPalette(blue); stripes.start();
        } else { stripes.stop(); setPalette(normal); }
        setToolTip(active ? "Old positions keep their takes. Each fully closed old cycle updates the free grid at the current price. New fills keep the ordinary repeat rules." : "");
        setProperty("transitionActive", active); update();
    }
protected:
    void paintEvent(QPaintEvent* event) override {
        if (!stripes.isActive()) { QPushButton::paintEvent(event); return; }
        QStylePainter painter(this); QStyleOptionButton option; initStyleOption(&option);
        painter.drawControl(QStyle::CE_PushButtonBevel, option);
        painter.save(); painter.setClipRect(rect().adjusted(3, 3, -3, -3));
        painter.setPen(Qt::NoPen); painter.setBrush(QColor(37, 99, 235, 18));
        for (int x = -height() - 24 + offset; x < width(); x += 24)
            painter.drawPolygon(QPolygon{QPoint(x, height()), QPoint(x + 10, height()), QPoint(x + height() + 10, 0), QPoint(x + height(), 0)});
        painter.restore(); painter.drawControl(QStyle::CE_PushButtonLabel, option);
    }
};
}
QString Window::text(const std::string& s) { return QString::fromStdString(s); }
double Window::price(Int value) { return static_cast<double>(value) / Scale; }
QString Window::amount(Int lots, const Spec& s) { return QString::number(static_cast<double>(lots) * s.quantityStep / Scale, 'f', 8); }
QString Window::role(Role r) { return r == Role::Entry ? "Entry" : "Take"; }
void Window::setupUi() {
    setWindowTitle("Deribit Grid — " + text(engine.config.instrument) + " — " + QFileInfo(profilePath).baseName()); resize(1250, 850);
    auto* root = new QWidget; auto* layout = new QVBoxLayout(root); setCentralWidget(root);
    auto* connection = new QHBoxLayout;
    instrumentBox = new QComboBox; instrumentBox->addItems({"BTC_USDC-PERPETUAL", "ETH_USDC-PERPETUAL"});
    environmentBox = new QComboBox; environmentBox->addItems({"Testnet", "Production"});
    idBox = new QLineEdit; idBox->setPlaceholderText("Client ID"); secretBox = new QLineEdit; secretBox->setEchoMode(QLineEdit::Password); secretBox->setPlaceholderText("Client secret (memory only)");
    connectButton = new QPushButton("Connect"); connectionLabel = new QLabel("Disconnected");
    connectButton->setObjectName("connect"); connectionLabel->setObjectName("connectionStatus");
    idBox->setObjectName("clientId"); secretBox->setObjectName("clientSecret");
    connection->addWidget(instrumentBox); connection->addWidget(environmentBox);
    connection->addWidget(new QLabel("Client ID:")); connection->addWidget(idBox);
    connection->addWidget(new QLabel("Secret:")); connection->addWidget(secretBox);
    connection->addWidget(connectButton); connection->addWidget(connectionLabel); layout->addLayout(connection);
    settings = new QTableWidget(2, 5); settings->setMaximumHeight(118);
    settings->setHorizontalHeaderLabels({"Side", "Lower bound (USDC)", "Upper bound (USDC)", "Entry notional cap (USDC)", "Intervals / slots"});
    settings->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); settings->verticalHeader()->hide();
    for (int row = 0; row < 2; ++row) {
        cell(settings, row, 0, row == 0 ? "Long" : "Short"); settings->item(row, 0)->setFlags(Qt::ItemIsEnabled);
        for (int col = 1; col <= 3; ++col) {
            auto* spin = new QDoubleSpinBox; spin->setRange(0, col == 3 ? 1e9 : 1e7); spin->setDecimals(col == 3 ? 2 : 8);
            spin->setSingleStep(col == 3 ? 10 : 1); spin->setGroupSeparatorShown(true); settings->setCellWidget(row, col, spin);
        }
        auto* n = new QSpinBox; n->setRange(1, 200); settings->setCellWidget(row, 4, n);
    }
    layout->addWidget(settings); setConfig(engine.config);
    auto* buttons = new QHBoxLayout;
    startButton = new QPushButton("Start / Resume"); pauseButton = new QPushButton("Pause (keep takes)");
    startButton->setObjectName("start"); pauseButton->setObjectName("pause");
    applyButton = new RebuildButton("Apply settings / Rebuild"); applyButton->setObjectName("applySettings");
    cancelButton = new QPushButton("Cancel all bot orders");
    cancelButton->setObjectName("cancelBotOrders");
    previewBox = new QCheckBox("Preview pending settings");
    auto* resetStatsButton = new QPushButton("Reset stats"); resetStatsButton->setObjectName("resetStats");
    resetStatsButton->setToolTip("Start a new count of P&L, fees and closed trades. Trading, orders and positions continue unchanged.");
    connect(resetStatsButton, &QPushButton::clicked, this, &Window::resetStats);
    buttons->addWidget(startButton); buttons->addWidget(pauseButton); buttons->addWidget(applyButton); buttons->addWidget(cancelButton); buttons->addWidget(previewBox); buttons->addStretch(); buttons->addWidget(resetStatsButton); layout->addLayout(buttons);
    auto* statusRow = new QHBoxLayout;
    summaryLabel = new QLabel; summaryLabel->setObjectName("gridSummary"); summaryLabel->setWordWrap(true); statusRow->addWidget(summaryLabel, 1);
    statsLabel = new QLabel; statsLabel->setObjectName("sessionStats");
    statsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statsLabel->setToolTip("P&L is gross, using grid entry/take prices, excluding fees and funding. Includes missed fills recovered on connection. Fees are actual USDC fees for both entries and takes; negative fees are rebates. Incomplete means some fees are unavailable. Each fully executed take order counts once. Counters reset when this window closes or Reset stats is clicked.");
    statusRow->addWidget(statsLabel); layout->addLayout(statusRow);
    auto* tabs = new QTabWidget;
    cyclesTable = new QTableWidget; setupTable(cyclesTable, {"Cycle", "Side", "Generation", "Entry", "Take", "Capacity (coin)", "Open (coin)", "Reserved USDC", "Mode"});
    ordersTable = new QTableWidget; setupTable(ordersTable, {"Cycle / Owner", "Role / Side", "Price", "Total (coin)", "Filled (coin)", "State", "Exchange ID"});
    previewTable = new QTableWidget; setupTable(previewTable, {"Side / Level", "Entry", "Take", "Quantity (coin)", "Notional (USDC)", "Status"});
    tabs->addTab(cyclesTable, "Cycles"); tabs->addTab(ordersTable, "Orders"); tabs->addTab(previewTable, "Current grid / Preview"); layout->addWidget(tabs, 1);
    auto* historyButtons = new QHBoxLayout;
    auto* clearLogButton = new QPushButton("Clear log"); clearLogButton->setObjectName("clearLog");
    resetButton = new QPushButton("Reset after manual trades"); resetButton->setObjectName("resetHistory");
    resetButton->setToolTip("Cancel all bot orders first. Clears saved cycles/orders and tracked inventory, keeping settings and session counters. Exchange position stays open.");
    historyButtons->addWidget(clearLogButton); historyButtons->addWidget(resetButton); historyButtons->addStretch(); layout->addLayout(historyButtons);
    journal = new QPlainTextEdit; journal->setReadOnly(true); journal->setMaximumBlockCount(1000); journal->setMaximumHeight(155); layout->addWidget(journal);
    auto* footer = new QHBoxLayout;
    latencyLabel = new QLabel; latencyLabel->setObjectName("connectionLatency");
    latencyLabel->setToolTip("Exchange response time in milliseconds. Red means disconnected, a response or connection taking at least 2 seconds, or no incoming data for 15 seconds.");
    footer->addWidget(latencyLabel); footer->addStretch();
    deleteProfileButton = new QPushButton("Delete current profile"); deleteProfileButton->setObjectName("deleteProfile");
    auto deletePalette = deleteProfileButton->palette(); deletePalette.setColor(QPalette::ButtonText, QColor("#b91c1c"));
    deletePalette.setColor(QPalette::Button, QColor("#fee2e2")); deleteProfileButton->setPalette(deletePalette);
    deleteProfileButton->setToolTip("Cancel this profile's exchange orders, then clear only its saved settings and history. Exchange positions remain open.");
    footer->addWidget(deleteProfileButton); layout->addLayout(footer);
    connect(deleteProfileButton, &QPushButton::clicked, this, [this] { cancelProfileOrders(true); });
    connect(clearLogButton, &QPushButton::clicked, journal, &QPlainTextEdit::clear);
    connect(resetButton, &QPushButton::clicked, this, &Window::resetHistory);
    connect(connectButton, &QPushButton::clicked, this, &Window::connectExchange);
    connect(startButton, &QPushButton::clicked, this, [this] { startGrid(); });
    connect(applyButton, &QPushButton::clicked, this, [this] { startGrid(true); });
    connect(pauseButton, &QPushButton::clicked, this, [this] { engine.pause(); save(); log("Pausing: cancelling entries and protecting fills"); render(); });
    connect(cancelButton, &QPushButton::clicked, this, [this] {
        if (!ready) { cancelProfileOrders(false); return; }
        if (QMessageBox::question(this, "Cancel bot orders", "Cancel every entry and take of this profile? The position will remain open.") != QMessageBox::Yes) return;
        engine.cancelAll(); save(); log("Cancelling all bot orders; position stays open"); render();
    });
    // Instrument and environment belong to the selected profile.
    instrumentBox->setEnabled(false); environmentBox->setEnabled(false);
    connect(instrumentBox, &QComboBox::currentTextChanged, this, [this] { render(); });
}
Config Window::uiConfig() const {
    Config c; c.instrument = instrumentBox->currentText().toStdString(); c.production = environmentBox->currentIndex() == 1;
    for (int row = 0; row < 2; ++row) {
        auto& b = row == 0 ? c.longs : c.shorts;
        b.low = fixed(qobject_cast<QDoubleSpinBox*>(settings->cellWidget(row, 1))->value());
        b.high = fixed(qobject_cast<QDoubleSpinBox*>(settings->cellWidget(row, 2))->value());
        b.budget = static_cast<Int>(std::llround(qobject_cast<QDoubleSpinBox*>(settings->cellWidget(row, 3))->value() * Dollars));
        b.levels = qobject_cast<QSpinBox*>(settings->cellWidget(row, 4))->value();
    }
    const auto error = Engine::validate(c); if (!error.empty()) throw std::runtime_error(error); return c;
}
void Window::setConfig(const Config& c) {
    instrumentBox->setCurrentText(text(c.instrument)); environmentBox->setCurrentIndex(c.production ? 1 : 0);
    environmentBox->setStyleSheet(environmentBox->currentIndex() ? "color:#b91c1c;font-weight:bold" : "");
    for (int row = 0; row < 2; ++row) {
        const auto& b = row == 0 ? c.longs : c.shorts;
        qobject_cast<QDoubleSpinBox*>(settings->cellWidget(row, 1))->setValue(price(b.low));
        qobject_cast<QDoubleSpinBox*>(settings->cellWidget(row, 2))->setValue(price(b.high));
        qobject_cast<QDoubleSpinBox*>(settings->cellWidget(row, 3))->setValue(static_cast<double>(b.budget) / Dollars);
        qobject_cast<QSpinBox*>(settings->cellWidget(row, 4))->setValue(b.levels);
    }
}
void Window::log(const QString& message) { journal->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss") + "  " + message); }
void Window::updateLatency() {
    const auto status = client.connectionStatus();
    using State = Client::ConnectionStatus::State;
    QString label = "Latency: — ms";
    if (status.state == State::Offline) label = "Latency: offline";
    else if (status.state == State::Connecting) label = QString("Connecting: %1 ms").arg(status.milliseconds);
    else if (status.milliseconds >= 0) label = QString("Latency: %1%2 ms").arg(status.waiting ? "≥ " : "").arg(status.milliseconds);
    if (latencyLabel->text() != label) latencyLabel->setText(label);
    const QString style = status.slow || status.state == State::Offline ? "color:#b91c1c" : "";
    if (latencyLabel->styleSheet() != style) latencyLabel->setStyleSheet(style);
}
void Window::render() {
    if (!summaryLabel) return;
    updateLatency();
    connectButton->setEnabled(!busy && !storageFailed && !closing); connectButton->setText(client.authenticated() ? "Reconnect / Reconcile" : "Connect");
    startButton->setEnabled(ready && !syncing && !busy && !engine.positionBlocked && !storageFailed && !closing && marketClock.isValid() && marketClock.elapsed() <= 10000);
    resetButton->setEnabled(ready && !syncing && !busy && !closing && !storageFailed && engine.canClearHistory());
    applyButton->setEnabled(startButton->isEnabled()); pauseButton->setEnabled(ready || engine.running || engine.rebuilding);
    const bool canCancel = client.authenticated() && accountLock && !busy && !storageFailed && !closing;
    cancelButton->setEnabled(canCancel); deleteProfileButton->setEnabled(canCancel);
    idBox->setEnabled(!client.authenticated()); secretBox->setEnabled(!client.authenticated());
    connectionLabel->setText(syncing ? "Reconciling" : ready ? "Connected" : client.authenticated() ? "Needs reconciliation" : "Disconnected");
    QString mode = engine.positionBlocked ? "Position mismatch" : engine.cancelEverything ? "Cancelling all" : engine.blocked ? "Blocked" : engine.rebuilding && !engine.running ? "Rebuilding" : engine.running ? "Running" : "Paused";
    QStringList transitions;
    for (const auto side : {Side::Long, Side::Short}) if (engine.transitioning(side)) {
        const auto& range = side == Side::Long ? engine.config.longs : engine.config.shorts;
        transitions.append(QString("%1 %2/%3").arg(text(sideName(side))).arg(engine.transitionSlots(side)).arg(range.levels));
    }
    if (!transitions.isEmpty()) mode += " [transition: " + transitions.join(", ") + "]";
    static_cast<RebuildButton*>(applyButton)->setTransition(!transitions.isEmpty());
    bool feesComplete = false;
    try { feesComplete = !feesUnavailable && store->feesComplete(); } catch (const std::exception&) { feesUnavailable = true; }
    statsLabel->setText(QString("Approx. P&L: %1 USDC\nFees (entry + take): %2 USDC%3\nClosed trades: %4")
        .arg(sessionPnl, 0, 'f', 4).arg(sessionFees, 0, 'f', 4).arg(feesComplete ? "" : " (incomplete)").arg(closedTrades));
    const QString grids = engine.config.longs.budget && engine.config.shorts.budget ? "Long + Short" : engine.config.longs.budget ? "Long" : engine.config.shorts.budget ? "Short" : "Disabled";
    summaryLabel->setText(QString("%1 | Grids: %2 | Mid: %3 | Net position: %4 coin\nLong reserved: %5 / %6 USDC (%7 / %8 slots) | Short reserved: %9 / %10 USDC (%11 / %12 slots)%13")
        .arg(mode, grids, QString::number(price(engine.market), 'f', 4), QString::number(netPosition, 'f', 8))
        .arg(money(engine.used(Side::Long)), money(engine.config.longs.budget)).arg(engine.availableSlots(Side::Long)).arg(engine.config.longs.levels)
        .arg(money(engine.used(Side::Short)), money(engine.config.shorts.budget)).arg(engine.availableSlots(Side::Short)).arg(engine.config.shorts.levels)
        .arg(engine.problem.empty() ? "" : "\n" + text(engine.problem)));
    int cycleRow = 0;
    for (const auto& c : engine.cycles()) {
        if (c.retired && c.held == 0 && !engine.hasLiveOrders(c.id)) continue;
        const int row = cycleRow++; if (row >= cyclesTable->rowCount()) cyclesTable->insertRow(row);
        const QStringList values{QString::number(c.id), text(sideName(c.side)), QString::number(c.generation), QString::number(price(c.entry), 'f', 4),
            QString::number(price(c.take), 'f', 4), amount(c.capacity, engine.spec), amount(c.held, engine.spec), money(engine.nominal(c.retired ? c.held : c.capacity, c.entry)), c.retired ? "Old: close only" : "Current: repeat"};
        for (int col = 0; col < values.size(); ++col) cell(cyclesTable, row, col, values[col]);
    }
    cyclesTable->setRowCount(cycleRow);
    int orderRow = 0;
    for (const auto& o : engine.orders()) {
        if (!o.live() && o.mutation == Mutation::None && o.status != Status::Rejected) continue;
        const int row = orderRow++; if (row >= ordersTable->rowCount()) ordersTable->insertRow(row);
        const QStringList values{QString::number(o.cycle), role(o.role) + (engine.buys(o) ? " / Buy" : " / Sell"), QString::number(price(o.price), 'f', 4),
            amount(o.total, engine.spec), amount(o.filled, engine.spec), state(o.status), text(o.exchangeId)};
        for (int col = 0; col < values.size(); ++col) cell(ordersTable, row, col, values[col]);
    }
    for (const auto& f : engine.foreign) {
        const int row = orderRow++; if (row >= ordersTable->rowCount()) ordersTable->insertRow(row);
        cell(ordersTable, row, 0, "Other / Manual"); cell(ordersTable, row, 1, (f.buy ? QString("Buy") : QString("Sell")) + (f.conditional ? " / Trigger" : "")); cell(ordersTable, row, 2, f.price > 0 ? QString::number(price(f.price), 'f', 4) : "Market on trigger");
        cell(ordersTable, row, 3, ""); cell(ordersTable, row, 4, "");
        cell(ordersTable, row, 5, "Untouched"); cell(ordersTable, row, 6, text(f.id));
    }
    ordersTable->setRowCount(orderRow);
    if (previewBox->isChecked()) {
        try { renderPreview(engine.preview(uiConfig(), true)); }
        catch (const std::exception& e) {
            previewTable->setRowCount(1); for (int col = 0; col < previewTable->columnCount(); ++col) cell(previewTable, 0, col, "");
            cell(previewTable, 0, 5, QString::fromUtf8(e.what()));
        }
    } else renderPreview(engine.currentPreview());
}
void Window::renderPreview(const Preview& view) {
    previewTable->setRowCount(static_cast<int>(view.lattice.size()));
    int index = 0;
    for (const auto& l : view.lattice) {
        const bool existing = view.allocated[static_cast<size_t>(index)];
        const int free = view.freeSlots[static_cast<size_t>(l.side)];
        const Int q = view.quantities[static_cast<size_t>(index)];
        cell(previewTable, index, 0, text(sideName(l.side)) + " / " + QString::number(index + 1)); cell(previewTable, index, 1, QString::number(price(l.entry), 'f', 4));
        cell(previewTable, index, 2, QString::number(price(l.take), 'f', 4)); cell(previewTable, index, 3, amount(q, view.spec));
        cell(previewTable, index, 4, money(view.notionals[static_cast<size_t>(index)])); cell(previewTable, index, 5, existing ? "Allocated" : free > 0 ? "Waiting / Available" : "Waiting for a slot"); ++index;
    }
}
