#include "window.h"
#include "api_numbers.h"
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QMessageBox>
#include <QSpinBox>
#include <QStylePainter>
#include <QStyleOptionButton>
#include <QSplitter>
#include <QTabWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <stdexcept>

using namespace grid;
using namespace deribit;
namespace {
QString text(const std::string& s) { return QString::fromStdString(s); }
double price(Int value) { return static_cast<double>(value) / Scale; }
double accountPosition(const QJsonObject& position) {
    const auto value = position["size_currency"];
    if (!value.isDouble() || !std::isfinite(value.toDouble())) throw std::runtime_error("Missing account position in base currency");
    return value.toDouble();
}
QString positionDetails(const Engine& engine, Int actual) {
    const Int tracked = engine.trackedPosition();
    const Int external = engine.externalPosition.value_or(tracked == 0 ? actual : 0);
    const double expected = price(tracked) + price(external);
    return QString("actual=%1 coin; bot=%2 coin; external=%3 coin; expected=%4 coin; difference=%5 coin")
        .arg(price(actual), 0, 'f', 8).arg(price(tracked), 0, 'f', 8).arg(price(external), 0, 'f', 8)
        .arg(expected, 0, 'f', 8).arg(price(actual) - expected, 0, 'f', 8);
}
QString amount(Int lots, const Spec& s) { return QString::number(static_cast<double>(lots) * s.quantityStep / Scale, 'f', 8); }
QString money(Int value) { return QString::number(static_cast<double>(value) / Dollars, 'f', 2); }
QString role(Role r) { return r == Role::Entry ? "Entry" : "Take"; }
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
Window::Window(const QString& profile, const QString& initialInstrument, QWidget* parent, const QUrl& endpoint, bool initialProduction) : QMainWindow(parent), client(this), testEndpoint(endpoint) {
    profilePath = QFileInfo(profile).absoluteFilePath();
    QDir().mkpath(QFileInfo(profile).absolutePath());
    profileLock = std::make_unique<QLockFile>(profile + ".lock");
    if (!profileLock->tryLock()) throw std::runtime_error("This profile is already open in another instance");
    store = std::make_unique<Store>(profile);
    if (!store->load(engine)) {
        engine.profileId = QUuid::createUuid().toString(QUuid::Id128).left(16).toStdString();
        engine.config.instrument = initialInstrument.toStdString();
        engine.config.production = initialProduction;
        const auto error = Engine::validate(engine.config); if (!error.empty()) throw std::runtime_error(error);
    }
    controller = std::make_unique<TradeController>(engine, *store, client);
    engine.pause(); engine.disconnected(); store->save(engine);
    setWindowTitle("Deribit Grid — " + text(engine.config.instrument) + " — " + QFileInfo(profile).baseName()); resize(1250, 850);
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
    statsLabel->setToolTip("P&L is gross, using grid entry/take prices, excluding fees and funding. Fees are actual USDC fees for both entries and takes since the first connection or last stats reset; negative fees are rebates. Incomplete means some fees are unavailable. Each fully executed take order counts once. Counters reset when this window closes or Reset stats is clicked.");
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
    connect(startButton, &QPushButton::clicked, this, &Window::startGrid);
    connect(applyButton, &QPushButton::clicked, this, &Window::startGrid);
    connect(pauseButton, &QPushButton::clicked, this, [this] { engine.pause(); save(); log("Pausing: cancelling entries and protecting fills"); render(); });
    connect(cancelButton, &QPushButton::clicked, this, [this] {
        if (!ready) { cancelProfileOrders(false); return; }
        if (QMessageBox::question(this, "Cancel bot orders", "Cancel every entry and take of this profile? The position will remain open.") != QMessageBox::Yes) return;
        engine.cancelAll(); save(); log("Cancelling all bot orders; position stays open"); render();
    });
    connect(&client, &Client::notice, this, &Window::log);
    connect(&client, &Client::lost, this, [this] {
        ++session; ready = false; syncing = false; busy = false; recoveryCancelling = false; readBusy = false; feeRequests.clear(); engine.disconnected(); save();
        log("Disconnected: exchange orders remain; waiting for reconciliation"); render();
    });
    connect(&client, &Client::authenticatedSession, this, &Window::initialize);
    connect(&client, &Client::subscription, this, [this](const QString& channel, const QJsonValue& payload) {
        try {
            if (channel.startsWith("ticker.")) readMarket(payload.toObject());
            else if (channel.startsWith("user.changes.")) {
                const auto object = payload.toObject(); const auto events = object["orders"].toArray();
                if (syncing) for (const auto& o : events) bufferedOrders.append(o); else receiveOrders(events);
                const auto trades = object["trades"].toArray();
                if (syncing) for (const auto& t : trades) bufferedTrades.append(t); else receiveTrades(trades);
                for (const auto& v : object["positions"].toArray()) {
                    const auto p = v.toObject(); if (p["instrument_name"].toString() == text(engine.config.instrument)) netPosition = accountPosition(p);
                }
                if (!object["trades"].toArray().isEmpty() && !syncing && !readBusy) refreshAccount();
            }
            render();
        } catch (const std::exception& e) { fail(QString::fromUtf8(e.what())); }
    });
    connect(instrumentBox, &QComboBox::currentTextChanged, this, [this] { render(); });
    pumpTimer.setInterval(400); connect(&pumpTimer, &QTimer::timeout, this, &Window::pump); pumpTimer.start();
    connect(&pumpTimer, &QTimer::timeout, this, &Window::updateLatency);
    readTimer.setInterval(5000); connect(&readTimer, &QTimer::timeout, this, &Window::refreshAccount); readTimer.start();
    log("Ready. Set ranges and budgets (0 disables a side), then connect. No market or stop-loss orders."); render();
}
Window::~Window() { client.stop(); }
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
    for (int row = 0; row < 2; ++row) {
        const auto& b = row == 0 ? c.longs : c.shorts;
        qobject_cast<QDoubleSpinBox*>(settings->cellWidget(row, 1))->setValue(price(b.low));
        qobject_cast<QDoubleSpinBox*>(settings->cellWidget(row, 2))->setValue(price(b.high));
        qobject_cast<QDoubleSpinBox*>(settings->cellWidget(row, 3))->setValue(static_cast<double>(b.budget) / Dollars);
        qobject_cast<QSpinBox*>(settings->cellWidget(row, 4))->setValue(b.levels);
    }
}
void Window::save() {
    if (storageFailed) return;
    try { store->save(engine); }
    catch (const std::exception& e) {
        storageFailed = true; ready = false; engine.pause(); engine.blocked = true;
        engine.problem = std::string("Storage failure; trading disabled: ") + e.what();
        log(text(engine.problem)); client.stop();
    }
}
void Window::log(const QString& message) { journal->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss") + "  " + message); }
void Window::fail(const QString& message) { engine.blocked = true; engine.problem = message.toStdString(); log(message); save(); render(); }
void Window::connectExchange() {
    try {
        if (idBox->text().trimmed().isEmpty() || secretBox->text().isEmpty()) throw std::runtime_error("Enter Client ID and client secret");
        auto next = uiConfig();
        if (!engine.cycles().empty() && (next.instrument != engine.config.instrument || next.production != engine.config.production))
            throw std::runtime_error("Use a separate profile for another instrument or environment");
        if (next.production && QMessageBox::question(this, "Production", "Connect to Production? Start / Resume can place real orders.") != QMessageBox::Yes) return;
        if (engine.cycles().empty()) engine.config = next;
        save(); if (storageFailed) return;
        ++session; ready = false; log(next.production ? "Connecting to Production" : "Connecting to Testnet");
        client.connectTo(idBox->text().trimmed(), secretBox->text(), next.production, testEndpoint); render();
    } catch (const std::exception& e) { QMessageBox::warning(this, "Configuration", QString::fromUtf8(e.what())); }
}
void Window::initialize(const QJsonObject&) {
    ++session; const int epoch = session; syncing = true; ready = false; bufferedOrders = {}; bufferedTrades = {}; feeRequests.clear();
    log("Authenticated; checking instrument, account and cancel-on-disconnect");
    client.call("public/get_instrument", {{"instrument_name", text(engine.config.instrument)}}, [this, epoch](const RpcResult& r) {
        if (epoch != session) return;
        if (!r.ok) { fail(r.error); return; }
        try {
            const auto o = r.value.toObject();
            if (o["instrument_name"].toString() != text(engine.config.instrument) || o["settlement_currency"].toString() != "USDC" ||
                o["base_currency"].toString() != (engine.config.instrument.starts_with("BTC") ? "BTC" : "ETH") || o["kind"].toString() != "future" ||
                o["settlement_period"].toString() != "perpetual" || o["state"].toString() != "open") throw std::runtime_error("Instrument is not a tradable USDC linear perpetual");
            Spec next{fixed(o["tick_size"].toDouble()), fixed(o["min_trade_amount"].toDouble()), {}};
            if (next.priceTick <= 0 || next.quantityStep <= 0) throw std::runtime_error("Invalid instrument specification");
            if (!engine.cycles().empty() && engine.spec.quantityStep && engine.spec.quantityStep != next.quantityStep) throw std::runtime_error("Quantity step changed; existing state requires manual review");
            for (const auto& v : o["tick_size_steps"].toArray()) {
                const auto step = v.toObject();
                if (!step["above_price"].isDouble() || !step["tick_size"].isDouble() || step["tick_size"].toDouble() <= 0)
                    throw std::runtime_error("Invalid tick step specification");
                next.tickSteps.emplace_back(fixed(step["above_price"].toDouble()), fixed(step["tick_size"].toDouble()));
            }
            engine.spec = next; save();
            log(QString("Instrument: tick=%1, quantity step=%2 coin, contract size=%3 coin")
                .arg(o["tick_size"].toDouble(), 0, 'g', 12).arg(o["min_trade_amount"].toDouble(), 0, 'g', 12).arg(o["contract_size"].toDouble(), 0, 'g', 12));
            client.call("private/get_account_summary", {{"currency", "USDC"}, {"extended", true}}, [this, epoch](const RpcResult& a) {
                if (epoch != session) return;
                if (!a.ok) { fail(a.error); return; }
                try {
                    const auto o = a.value.toObject(); const auto id = o["id"];
                    client.setTradingLimits(o["limits"].toObject());
                    if ((!id.isString() && !id.isDouble()) || (id.isString() && id.toString().isEmpty()) || (id.isDouble() && id.toInteger() <= 0)) throw std::runtime_error("Account ID is missing from the extended account summary");
                    accountId = id.isString() ? id.toString() : QString::number(id.toInteger());
                    const QString identity = accountId + "/" + (engine.config.production ? "prod/" : "test/") + text(engine.config.instrument);
                    store->bindAccount(identity);
                    const auto hash = QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex();
                    if (!accountLock) {
                        accountLock = std::make_unique<QLockFile>(QDir::tempPath() + "/deribit-grid-" + QString::fromLatin1(hash) + ".lock");
                        if (!accountLock->tryLock()) { accountLock.reset(); throw std::runtime_error("Another local bot is using this account/instrument"); }
                    }
                    client.call("private/disable_cancel_on_disconnect", {{"scope", "connection"}}, [this, epoch](const RpcResult& cod) {
                        if (epoch != session) return;
                        if (!cod.ok) { fail("Cannot disable connection cancel-on-disconnect: " + cod.error); return; }
                        const QString instrument = text(engine.config.instrument);
                        const QJsonArray channels{"ticker." + instrument + ".100ms", "user.changes." + instrument + ".100ms"};
                        client.call("private/subscribe", {{"channels", channels}}, [this, epoch](const RpcResult& sub) {
                            if (epoch != session) return;
                            if (!sub.ok) { fail(sub.error); return; } synchronize();
                        });
                    });
                } catch (const std::exception& ex) { fail(QString::fromUtf8(ex.what())); client.stop(); }
            });
        } catch (const std::exception& e) { fail(QString::fromUtf8(e.what())); }
    });
}
void Window::synchronize() {
    syncing = true; ready = false; positionChecks = 0; reconcileLabels.clear(); reconcileIndex = 0; reconcileSnapshots.clear(); absentCreates.clear();
    for (const auto& o : engine.orders()) if (o.live() || o.mutation != Mutation::None) reconcileLabels.push_back(o.label);
    // Classify once; preserve notification order within each role.
    std::stable_partition(reconcileLabels.begin(), reconcileLabels.end(), [this](const auto& label) {
        const auto* o = engine.order(label); return o && o->role == Role::Entry;
    });
    const int epoch = session;
    client.call("private/get_open_orders_by_instrument", {{"instrument_name", text(engine.config.instrument)}}, [this, epoch](const RpcResult& r) {
        if (epoch != session) return;
        if (!r.ok) { fail(r.error); return; }
        try {
            if (!r.value.isArray()) throw std::runtime_error("Invalid open order list during reconciliation");
            engine.foreign.clear();
            for (const auto& v : r.value.toArray()) {
                const auto o = v.toObject(); const auto label = o["label"].toString();
                if (engine.order(label.toStdString())) {
                    if (reconcileSnapshots.contains(label)) throw std::runtime_error("Duplicate bot label in open order list");
                    reconcileSnapshots.insert(label, o);
                } else {
                    if (label.startsWith("dg_" + text(engine.profileId) + "_")) throw std::runtime_error("Untracked order with this profile label; manual review required");
                    receiveForeignOrder(o);
                }
            }
            reconcileNext();
        } catch (const std::exception& ex) { syncing = false; fail(QString::fromUtf8(ex.what())); }
    });
}
void Window::cancelProfileOrders(bool eraseProfile) {
    const QString message = eraseProfile
        ? "Cancel all entries and takes of THIS profile, then delete its saved settings and history? Other profiles are unchanged. Any exchange position remains open WITHOUT take protection. A fresh empty profile will be created in the same file."
        : "Cancel all entries and takes of THIS profile despite the reconciliation error? The exchange position will remain open. Other profiles and manual orders are unchanged.";
    if (QMessageBox::warning(this, eraseProfile ? "Delete current profile: " + QFileInfo(profilePath).fileName() : "Cancel bot orders", message, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    if (!client.authenticated() || !accountLock || busy || recoveryCancelling || storageFailed) { log("Connect to the bound account before cancelling or deleting this profile"); return; }
    // Invalidate incomplete read callbacks. Persist a cancel-only state before sending
    // any idempotent label cancellation; an interrupted attempt retains the profile.
    ++session; ready = false; syncing = true; busy = true; readBusy = false; recoveryCancelling = true;
    engine.cancelAll(); save(); if (storageFailed) return; render(); const int epoch = session;
    log("Cancelling exchange orders of this profile by exact labels; saved data retained until verification");
    client.call("private/get_open_orders_by_instrument", {{"instrument_name", text(engine.config.instrument)}}, [this, epoch, eraseProfile](const RpcResult& r) {
        if (epoch != session) return;
        try {
            if (!r.ok || !r.value.isArray()) throw std::runtime_error("Cannot list orders for cancellation: " + r.error.toStdString());
            QSet<QString> labels;
            for (const auto& o : engine.orders()) if (o.live() || o.mutation != Mutation::None) labels.insert(text(o.label));
            const auto prefix = "dg_" + text(engine.profileId) + "_";
            for (const auto& v : r.value.toArray()) {
                if (!v.isObject()) throw std::runtime_error("Invalid open order list");
                const auto o = v.toObject();
                if (o["instrument_name"].toString() != text(engine.config.instrument)) throw std::runtime_error("Wrong instrument in cancellation snapshot");
                if (o["label"].toString().startsWith(prefix)) labels.insert(o["label"].toString());
            }
            QStringList ordered(labels.begin(), labels.end()); ordered.sort(); cancelProfileLabels(ordered, 0, eraseProfile);
        } catch (const std::exception& ex) { busy = false; syncing = false; recoveryCancelling = false; fail(QString::fromUtf8(ex.what()) + "; profile retained"); }
    });
}
void Window::cancelProfileLabels(const QStringList& labels, int index, bool eraseProfile) {
    if (index >= labels.size()) { verifyProfileCancellation(eraseProfile, 0); return; }
    const int epoch = session;
    client.call("private/cancel_by_label", {{"currency", "USDC"}, {"label", labels[index]}}, [this, epoch, labels, index, eraseProfile](const RpcResult& r) {
        if (epoch != session) return;
        if (!r.ok || !r.value.isDouble() || r.value.toInteger(-1) < 0) {
            busy = false; syncing = false; recoveryCancelling = false;
            fail("Label cancellation not confirmed: " + (r.error.isEmpty() ? QString("invalid exchange response") : r.error) + "; profile retained. Reconnect / Reconcile, or retry Cancel all bot orders."); return;
        }
        cancelProfileLabels(labels, index + 1, eraseProfile);
    }, true);
}
void Window::verifyProfileCancellation(bool eraseProfile, int checks) {
    const int epoch = session;
    // A second empty snapshot catches an order accepted just after the first cancel.
    QTimer::singleShot(checks ? 2000 : 0, this, [this, epoch, eraseProfile, checks] {
        if (epoch != session) return;
        client.call("private/get_open_orders_by_instrument", {{"instrument_name", text(engine.config.instrument)}}, [this, epoch, eraseProfile, checks](const RpcResult& r) {
            if (epoch != session) return;
            try {
                if (!r.ok || !r.value.isArray()) throw std::runtime_error("Final cancellation check failed: " + r.error.toStdString());
                for (const auto& v : r.value.toArray()) {
                    if (!v.isObject() || v.toObject()["instrument_name"].toString() != text(engine.config.instrument) || v.toObject()["order_id"].toString().isEmpty())
                        throw std::runtime_error("Invalid final cancellation snapshot");
                    if (v.toObject()["label"].toString().startsWith("dg_" + text(engine.profileId) + "_"))
                        throw std::runtime_error("A profile order is still open; retry Cancel all bot orders");
                }
                if (checks == 0) { verifyProfileCancellation(eraseProfile, 1); return; }
                if (!eraseProfile) {
                    busy = false; syncing = false; recoveryCancelling = false;
                    log("All exchange orders of this profile cancelled; reconciling saved fills"); synchronize(); return;
                }
                client.call("private/get_position", {{"instrument_name", text(engine.config.instrument)}}, [this, epoch](const RpcResult& p) {
                    if (epoch != session) return;
                    try {
                        if (!p.ok) throw std::runtime_error("Cannot read remaining position: " + p.error.toStdString());
                        Engine fresh; fresh.config.instrument = engine.config.instrument; fresh.config.production = engine.config.production; fresh.spec = engine.spec;
                        fresh.profileId = QUuid::createUuid().toString(QUuid::Id128).left(16).toStdString();
                        netPosition = accountPosition(p.value.toObject()); fresh.externalPosition = signedFixed(netPosition);
                        store->resetProfile(fresh); engine = std::move(fresh); ++session; ++statsRevision;
                        absentCreates.clear(); bufferedOrders = {}; bufferedTrades = {}; feeRequests.clear();
                        sessionPnl = 0; sessionFees = 0; closedTrades = 0; feesUnavailable = false; statsReady = false;
                        busy = false; syncing = false; recoveryCancelling = false; setConfig(engine.config);
                        log("Current profile cleared and recreated; exchange position remains " + QString::number(netPosition, 'f', 8) + " coin. Set ranges and budgets before Start.");
                        synchronize();
                    } catch (const std::exception& ex) { busy = false; syncing = false; recoveryCancelling = false; fail(QString::fromUtf8(ex.what()) + "; profile retained"); }
                });
            } catch (const std::exception& ex) { busy = false; syncing = false; recoveryCancelling = false; fail(QString::fromUtf8(ex.what()) + "; profile retained"); }
        });
    });
}
void Window::reconcileNext() {
    // Reuse the bulk snapshot in entry-before-take order. Uncertain requests still need an explicit read.
    try {
        while (reconcileIndex < reconcileLabels.size()) {
            const auto& label = reconcileLabels[reconcileIndex]; const auto* o = engine.order(label);
            if (!o) throw std::runtime_error("Missing order during reconciliation");
            const auto snapshot = reconcileSnapshots.find(text(label));
            if (snapshot == reconcileSnapshots.end() || o->exchangeId.empty() || o->mutation != Mutation::None || o->status == Status::Uncertain) break;
            // No request is pending for these snapshots; completeSync commits the batch before trading.
            receiveOrder(snapshot.value(), false, false); ++reconcileIndex;
        }
    } catch (const std::exception& ex) { syncing = false; fail(QString::fromUtf8(ex.what())); return; }
    if (reconcileIndex >= reconcileLabels.size()) { completeSync(); return; }
    const auto label = reconcileLabels[reconcileIndex++]; const auto* o = engine.order(label); const int epoch = session;
    if (!o) { syncing = false; fail("Missing order during reconciliation"); return; }
    const QString method = o->exchangeId.empty() ? "private/get_order_state_by_label" : "private/get_order_state";
    const QJsonObject params = o->exchangeId.empty() ? QJsonObject{{"currency", "USDC"}, {"label", text(label)}} : QJsonObject{{"order_id", text(o->exchangeId)}};
    client.call(method, params, [this, epoch, label](const RpcResult& r) {
        if (epoch != session) return;
        // A lost create acknowledgement can mean either an accepted order or a request
        // which never reached the matching engine. Empty recent results alone prove neither.
        if (r.ok && r.value.isArray() && r.value.toArray().isEmpty()) {
            const auto snapshot = reconcileSnapshots.constFind(text(label));
            if (snapshot != reconcileSnapshots.cend()) {
                try { receiveOrder(snapshot.value(), true); reconcileNext(); }
                catch (const std::exception& ex) { syncing = false; fail(QString::fromUtf8(ex.what())); }
            } else reconcileHistory(label, false, 0, 0);
            return;
        }
        QJsonObject snapshot;
        if (r.value.isArray()) {
            const auto matches = r.value.toArray();
            if (matches.size() == 1) snapshot = matches[0].toObject();
        } else if (r.value.isObject()) snapshot = r.value.toObject();
        if (!r.ok || snapshot["order_id"].toString().isEmpty() || snapshot["label"].toString() != text(label)) {
            engine.uncertain(label); save(); syncing = false;
            fail("Cannot resolve saved order " + text(label) + ": " + (r.error.isEmpty() ? QString("invalid or ambiguous exchange response; use Reconnect / Reconcile to retry") : r.error)); return;
        }
        try { receiveOrder(snapshot, true); reconcileNext(); }
        catch (const std::exception& ex) { fail(QString::fromUtf8(ex.what())); syncing = false; }
    });
}
void Window::reconcileHistory(const std::string& label, bool historical, int offset, int pass) {
    const int epoch = session;
    client.call("private/get_order_history_by_instrument", {{"instrument_name", text(engine.config.instrument)},
        {"count", 1000}, {"offset", offset}, {"include_old", true}, {"include_unfilled", true}, {"historical", historical}},
        [this, epoch, label, historical, offset, pass](const RpcResult& r) {
        if (epoch != session) return;
        try {
            if (!r.ok || !r.value.isArray()) throw std::runtime_error(("Order history reconciliation failed: " +
                (r.error.isEmpty() ? QString("invalid exchange response") : r.error)).toStdString());
            const auto history = r.value.toArray(); QJsonObject match;
            for (const auto& v : history) {
                if (!v.isObject() || v.toObject()["order_id"].toString().isEmpty()) throw std::runtime_error("Invalid order history record");
                const auto order = v.toObject();
                if (order["label"].toString() != text(label)) continue;
                if (!match.isEmpty()) throw std::runtime_error("Duplicate bot label in order history");
                match = order;
            }
            if (!match.isEmpty()) { receiveOrder(match, true); reconcileNext(); return; }
            if (history.size() == 1000) {
                if (offset >= 99000) throw std::runtime_error("Order history search limit reached; missing request remains unresolved");
                reconcileHistory(label, historical, offset + 1000, pass); return;
            }
            if (!historical) { reconcileHistory(label, true, 0, pass); return; }
            const auto* o = engine.order(label);
            if (!o || !o->exchangeId.empty() || o->filled != 0 || o->mutation != Mutation::Create)
                throw std::runtime_error("Saved accepted order is missing from the exchange; manual review required");
            if (pass == 0) { confirmMissingCreate(label); return; }
            // Defer releasing this intent until buffered notifications and account position
            // have also been checked. Its label is never reused for a new request.
            absentCreates.push_back(label); reconcileNext();
        } catch (const std::exception& ex) { syncing = false; fail(QString::fromUtf8(ex.what())); }
    });
}
void Window::confirmMissingCreate(const std::string& label) {
    const int epoch = session;
    log("No exchange record for pending create " + text(label) + "; checking again before releasing its slot");
    QTimer::singleShot(2000, this, [this, epoch, label] {
        if (epoch != session) return;
        client.call("private/get_open_orders_by_instrument", {{"instrument_name", text(engine.config.instrument)}}, [this, epoch, label](const RpcResult& r) {
            if (epoch != session) return;
            try {
                if (!r.ok || !r.value.isArray()) throw std::runtime_error("Cannot confirm open orders for missing create: " + r.error.toStdString());
                QJsonObject match;
                for (const auto& v : r.value.toArray()) {
                    if (!v.isObject() || v.toObject()["order_id"].toString().isEmpty()) throw std::runtime_error("Invalid open order record");
                    if (v.toObject()["label"].toString() != text(label)) continue;
                    if (!match.isEmpty()) throw std::runtime_error("Duplicate bot label in open orders");
                    match = v.toObject();
                }
                if (!match.isEmpty()) { receiveOrder(match, true); reconcileNext(); }
                else reconcileHistory(label, false, 0, 1);
            } catch (const std::exception& ex) { syncing = false; fail(QString::fromUtf8(ex.what())); }
        });
    });
}
void Window::completeSync() {
    try {
        receiveOrders(bufferedOrders); bufferedOrders = {};
        const int epoch = session; const auto revision = positionRevision;
        client.call("private/get_position", {{"instrument_name", text(engine.config.instrument)}}, [this, epoch, revision](const RpcResult& r) {
            if (epoch != session) return;
            if (!r.ok) { syncing = false; fail("Position reconciliation failed: " + r.error); return; }
            // Drain batched order notifications before comparing the account snapshot.
            QTimer::singleShot(200, this, [this, epoch, revision, r] {
                if (epoch != session) return;
                try {
                    receiveOrders(bufferedOrders); bufferedOrders = {};
                    if (positionRevision != revision) { completeSync(); return; }
                    netPosition = accountPosition(r.value.toObject()); const Int actual = signedFixed(netPosition);
                    // Do not adopt an unexplained position as the initial external offset
                    // when deciding that an uncertain create was not accepted.
                    if (!absentCreates.empty() && (!engine.positionMatches(actual) || (!engine.externalPosition && actual != engine.trackedPosition()))) {
                        syncing = false; fail("Missing create cannot be released: account position does not match saved fills; use Reconnect / Reconcile after review"); return;
                    }
                    if (!engine.positionMatches(actual) && positionChecks++ < 2) { QTimer::singleShot(250, this, [this, epoch] { if (epoch == session) completeSync(); }); return; }
                    const bool matched = engine.reconcilePosition(actual);
                    if (matched) for (const auto& label : absentCreates) {
                        const auto* o = engine.order(label);
                        if (!o) throw std::runtime_error("Missing create during final reconciliation");
                        if (!o->exchangeId.empty()) { engine.resolved(label); continue; }
                        if (o->filled != 0 || o->mutation != Mutation::Create) throw std::runtime_error("Missing create changed during reconciliation");
                        engine.rejected(label, "Unaccepted create reconciled; review and Start / Resume");
                        log("Pending create absent from open orders and recent/historical records; released " + text(label) + ". Start / Resume is available.");
                    }
                    absentCreates.clear();
                    if (!matched) log("Position mismatch: " + positionDetails(engine, actual));
                    if (!statsReady) feesSince = QDateTime::currentMSecsSinceEpoch();
                    syncing = false; ready = true; statsReady = true; save();
                    log(matched ? QString("Reconciled own orders and account position. %1.").arg(engine.running ? "Running" : "Paused; Start / Resume rebuilds free slots") : text(engine.problem));
                    receiveTrades(bufferedTrades); bufferedTrades = {};
                    // Protect recovered fills before queuing fee/account reads or waiting for the next pump tick.
                    if (matched) pump();
                    try { for (const auto& id : store->missingFees()) refreshFees(id); } catch (const std::exception&) { feesUnavailable = true; }
                    refreshAccount(); render();
                } catch (const std::exception& e) { syncing = false; fail(QString::fromUtf8(e.what())); }
            });
        });
    } catch (const std::exception& e) { fail(QString::fromUtf8(e.what())); }
}
void Window::receiveOrder(const QJsonObject& json, bool resolve, bool persist, const std::string* label) {
    const auto receipt = controller->receiveOwnOrder(json, resolve, persist, label);
    if (!receipt) { receiveForeignOrder(json); return; }
    if (receipt->delta > 0) ++positionRevision;
    if (statsReady && receipt->delta > 0) {
        try { store->expectFee(receipt->exchangeId, receipt->delta); } catch (const std::exception&) { feesUnavailable = true; }
        QTimer::singleShot(250, this, [this, id = receipt->exchangeId] { refreshFees(id); });
    }
    if (statsReady && receipt->take) {
        sessionPnl += price(receipt->spread) * static_cast<double>(receipt->delta) * engine.spec.quantityStep / Scale;
        if (receipt->closed) ++closedTrades;
    }
}
void Window::receiveForeignOrder(const QJsonObject& j) {
    if (j["instrument_name"].toString() != text(engine.config.instrument)) return;
    const auto id = j["order_id"].toString().toStdString(); if (id.empty()) return;
    std::erase_if(engine.foreign, [&](const ForeignOrder& f) { return f.id == id; });
    const auto status = j["order_state"].toString();
    if (status == "open" || status == "untriggered" || status == "triggered") {
        const bool conditional = j["order_type"].toString() != "limit";
        const Int p = j["price"].isDouble() ? fixed(j["price"].toDouble()) : 0;
        engine.foreign.push_back({id, j["direction"].toString() == "buy", p, conditional, j["reduce_only"].toBool()});
    } else if (status == "cancelled") {
        log("Other / Manual order cancelled: " + text(id) + " | Exchange reason: " + j["cancel_reason"].toString("not provided"));
    }
}
void Window::receiveOrders(const QJsonArray& events) {
    for (const auto& order : controller->orderedNotifications(events)) {
        if (order.own) receiveOrder(order.json, false, false, &order.label);
        else receiveForeignOrder(order.json);
    }
    // Commit a notification batch once; every later trading mutation still saves its intent first.
    save();
}
void Window::receiveTrades(const QJsonArray& trades) {
    if (!statsReady) return;
    for (const auto& value : trades) {
        const auto t = value.toObject(); const QString id = t["order_id"].toString();
        if (id.isEmpty() || t["instrument_name"].toString() != text(engine.config.instrument) ||
            !t["timestamp"].isDouble() || t["timestamp"].toInteger() < feesSince) continue;
        const auto* labelled = engine.order(t["label"].toString().toStdString());
        bool own = false;
        try { own = (labelled && (labelled->exchangeId.empty() || text(labelled->exchangeId) == id)) ||
            engine.orderByExchangeId(id.toStdString()) != nullptr || store->ownsExchangeId(id);
        } catch (const std::exception&) { feesUnavailable = true; continue; }
        const QString tradeId = t["trade_id"].toString(); const QString key = id + "/" + tradeId;
        if (!own || tradeId.isEmpty() || !t["fee"].isDouble() ||
            t["fee_currency"].toString() != "USDC" || !std::isfinite(t["fee"].toDouble())) continue;
        // Fee data must never change inventory or prevent trading when unavailable/malformed.
        try {
            const Int lots = apiLots(t["amount"], engine.spec); if (lots <= 0) continue;
            if (store->recordFee(key, id, lots)) sessionFees += t["fee"].toDouble();
        } catch (const std::exception&) { feesUnavailable = true; }
    }
}
void Window::refreshFees(const QString& id) {
    try {
    if (feesUnavailable || !ready || syncing || !client.authenticated() || feeRequests.contains(id) || store->feeValue(id, true) >= store->feeValue(id)) return;
    feeRequests.insert(id); const int epoch = session; const Int expected = store->feeValue(id); const auto statsEpoch = statsRevision;
    client.call("private/get_user_trades_by_order", {{"order_id", id}, {"sorting", "asc"}}, [this, id, epoch, expected, statsEpoch](const RpcResult& r) {
        if (epoch != session || statsEpoch != statsRevision) return;
        try {
        feeRequests.remove(id);
        if (r.ok && r.value.isArray()) receiveTrades(r.value.toArray());
        else log("Fees unavailable for " + id + "; gross P&L remains available");
        render();
        // A fill received during this read needs a newer snapshot, but missing fees alone do not cause retries.
        if (store->feeValue(id) > expected) refreshFees(id);
        } catch (const std::exception&) { feesUnavailable = true; feeRequests.remove(id); }
    });
    } catch (const std::exception&) { feesUnavailable = true; }
}
void Window::readMarket(const QJsonObject& ticker) {
    if (ticker["instrument_name"].toString() != text(engine.config.instrument)) return;
    if (!ticker["best_bid_price"].isDouble() || !ticker["best_ask_price"].isDouble()) return;
    const Int b = fixed(ticker["best_bid_price"].toDouble()), a = fixed(ticker["best_ask_price"].toDouble());
    if (b <= 0 || a < b) return;
    engine.quote(b, a); marketClock.restart();
}
void Window::refreshAccount() {
    if (!ready || syncing || readBusy || !client.authenticated()) return;
    readBusy = true; const int epoch = session; const auto revision = positionRevision;
    client.call("private/get_position", {{"instrument_name", text(engine.config.instrument)}}, [this, epoch, revision](const RpcResult& r) {
        if (epoch != session) return;
        try {
            Int actual = 0;
            if (r.ok) {
                netPosition = accountPosition(r.value.toObject());
                actual = signedFixed(netPosition);
            } else log(r.error);
            const auto finish = [this, epoch, revision, actual, ok = r.ok] {
                if (epoch != session) return;
                try {
                    if (ok && ready && !syncing && !busy && !engine.positionBlocked && revision == positionRevision && !engine.positionMatches(actual)) {
                        readBusy = false; log("Account position differs from tracked fills; reconciling: " + positionDetails(engine, actual)); synchronize(); return;
                    }
                    client.call("public/ticker", {{"instrument_name", text(engine.config.instrument)}}, [this, epoch](const RpcResult& t) {
                        if (epoch != session) return; readBusy = false;
                        try { if (t.ok) readMarket(t.value.toObject()); else log(t.error); render(); }
                        catch (const std::exception& e) { fail(QString::fromUtf8(e.what())); }
                    });
                } catch (const std::exception& e) { readBusy = false; log(QString::fromUtf8(e.what())); }
            };
            // Account reads can overtake the aggregated fill notification; do not freeze takes on that first snapshot.
            if (r.ok && revision == positionRevision && !engine.positionMatches(actual)) QTimer::singleShot(200, this, finish);
            else finish();
        } catch (const std::exception& e) { readBusy = false; log(QString::fromUtf8(e.what())); return; }
    });
}
void Window::startGrid() {
    try {
        if (!ready || !marketClock.isValid() || marketClock.elapsed() > 10000) throw std::runtime_error("Wait for a fresh market quote and successful reconciliation");
        if (engine.cancelEverything && !engine.settled()) throw std::runtime_error("Wait until cancellation finishes");
        auto next = uiConfig();
        if (next.instrument != engine.config.instrument || next.production != engine.config.production) throw std::runtime_error("Reconnect with a separate profile for another instrument/environment");
        previewBox->setChecked(false); engine.requestRebuild(next); save(); log("Rebuild requested: preserving old takes and reallocating free slots"); render();
    } catch (const std::exception& e) { QMessageBox::warning(this, "Cannot start", QString::fromUtf8(e.what())); }
}
void Window::resetHistory() {
    if (!ready || syncing || busy || !engine.canClearHistory()) return;
    if (QMessageBox::question(this, "Reset after manual trades", "Clear saved cycles/orders, tracked inventory and the log? Settings and session counters stay. The exchange position stays open and is not adopted into bot cycles.") != QMessageBox::Yes) return;
    busy = true; render(); const int epoch = session;
    client.call("private/get_open_orders_by_instrument", {{"instrument_name", text(engine.config.instrument)}}, [this, epoch](const RpcResult& r) {
        if (epoch != session) return;
        if (!r.ok || !r.value.isArray()) { busy = false; fail("Cannot verify bot order cancellation: " + r.error); return; }
        for (const auto& v : r.value.toArray()) if (v.toObject()["label"].toString().startsWith("dg_" + text(engine.profileId) + "_")) {
            busy = false; fail("Bot orders remain on the exchange; cancel and reconcile before resetting"); return;
        }
        client.call("private/get_position", {{"instrument_name", text(engine.config.instrument)}}, [this, epoch](const RpcResult& p) {
            if (epoch != session) return; busy = false;
            try {
                if (!p.ok) throw std::runtime_error("Cannot verify account position before resetting");
                netPosition = accountPosition(p.value.toObject()); engine.clearHistory(signedFixed(netPosition)); save();
                if (!storageFailed) { journal->clear(); log("History and tracked inventory cleared. Settings and exchange position preserved. Start / Resume to build a fresh grid."); }
                render();
            } catch (const std::exception& e) { fail(QString::fromUtf8(e.what())); }
        });
    });
}
void Window::resetStats() {
    try {
        store->resetFees();
        ++statsRevision; feesSince = QDateTime::currentMSecsSinceEpoch(); feeRequests.clear();
        sessionPnl = 0; sessionFees = 0; closedTrades = 0; feesUnavailable = false;
        log("P&L, fees and closed trades reset. New statistics period started."); render();
    } catch (const std::exception& e) { log("Cannot reset statistics: " + QString::fromUtf8(e.what())); }
}
void Window::submit(const Action& a) {
    save(); if (storageFailed) return; busy = true;
    const QString method = a.kind == Mutation::Create ? (a.buy ? "private/buy" : "private/sell") : a.kind == Mutation::Edit ? "private/edit" : "private/cancel";
    const int epoch = session;
    log(method + " " + role(a.role) + " @ " + QString::number(price(a.price), 'f', 4) + (a.kind == Mutation::Cancel ? "" : " qty=" + amount(a.total, engine.spec)) + " | " + (a.exchangeId.empty() ? text(a.label) : text(a.exchangeId)));
    try { controller->execute(a, [this, epoch, a](const RpcResult& r) {
        if (epoch != session) return; busy = false;
        try {
            if (!r.ok) {
                if (r.uncertain) { engine.uncertain(a.label); save(); ready = false; synchronize(); }
                else {
                    engine.rejected(a.label, r.error.toStdString()); save(); log(r.error);
                    if (a.kind != Mutation::Create) { ready = false; synchronize(); }
                }
            } else {
                const auto root = r.value.toObject(); const auto order = root.contains("order") ? root["order"].toObject() : root;
                receiveOrder(order, true); receiveTrades(root["trades"].toArray()); save();
                QTimer::singleShot(0, this, &Window::pump);
            }
            render();
        } catch (const std::exception& e) { engine.uncertain(a.label); save(); log(QString::fromUtf8(e.what())); ready = false; synchronize(); }
    }); } catch (...) { busy = false; throw; }
}
void Window::pump() {
    if (!ready || syncing || busy || storageFailed || !client.authenticated()) return;
    try {
        if (!marketClock.isValid() || marketClock.elapsed() > 10000) {
            if (engine.running || engine.rebuilding) { engine.pause(); save(); log("Stale market data: cancelling entries; Resume after a fresh quote"); }
        }
        auto action = engine.nextAction();
        if (action) submit(*action);
        else {
            save();
            if (closing && engine.settled() && !engine.blocked) { closeAllowed = true; close(); }
        }
        render();
    } catch (const std::exception& e) { fail(QString::fromUtf8(e.what())); }
}
void Window::updateLatency() {
    const auto status = client.connectionStatus();
    using State = Client::ConnectionStatus::State;
    QString label = "Latency: — ms";
    if (status.state == State::Offline) label = "Latency: offline";
    else if (status.state == State::Connecting) label = QString("Connecting: %1 ms").arg(status.milliseconds);
    else if (status.milliseconds >= 0) label = QString("Latency: %1%2 ms").arg(status.waiting ? "≥ " : "").arg(status.milliseconds);
    latencyLabel->setText(label);
    latencyLabel->setStyleSheet(status.slow || status.state == State::Offline ? "color:#b91c1c" : "");
}
void Window::render() {
    if (!summaryLabel) return;
    updateLatency();
    // Instrument and environment belong to the selected profile and cannot change in place.
    instrumentBox->setEnabled(false); environmentBox->setEnabled(false);
    connectButton->setEnabled(!busy && !storageFailed && !closing); connectButton->setText(client.authenticated() ? "Reconnect / Reconcile" : "Connect");
    startButton->setEnabled(ready && !syncing && !busy && !engine.positionBlocked && !storageFailed && !closing && marketClock.isValid() && marketClock.elapsed() <= 10000);
    resetButton->setEnabled(ready && !syncing && !busy && !closing && !storageFailed && engine.canClearHistory());
    applyButton->setEnabled(startButton->isEnabled()); pauseButton->setEnabled(ready);
    const bool canCancel = client.authenticated() && accountLock && !busy && !storageFailed && !closing;
    cancelButton->setEnabled(canCancel); deleteProfileButton->setEnabled(canCancel);
    idBox->setEnabled(!client.authenticated()); secretBox->setEnabled(!client.authenticated());
    connectionLabel->setText(syncing ? "Reconciling" : ready ? "Connected" : client.authenticated() ? "Needs reconciliation" : "Disconnected");
    environmentBox->setStyleSheet(environmentBox->currentIndex() ? "color:#b91c1c;font-weight:bold" : "");
    QString mode = engine.positionBlocked ? "Position mismatch" : engine.cancelEverything ? "Cancelling all" : engine.blocked ? "Blocked" : engine.rebuilding && !engine.running ? "Rebuilding" : engine.running ? "Running" : "Paused";
    QStringList transitions;
    for (const auto side : {Side::Long, Side::Short}) if (engine.transitioning(side)) {
        const auto& range = side == Side::Long ? engine.config.longs : engine.config.shorts;
        transitions.append(QString("%1 %2/%3").arg(text(sideName(side))).arg(engine.transitionSlots(side)).arg(range.levels));
    }
    if (!transitions.isEmpty()) mode += " [transition: " + transitions.join(", ") + "]";
    static_cast<RebuildButton*>(applyButton)->setTransition(!transitions.isEmpty());
    applyButton->setToolTip(transitions.isEmpty() ? "" : "Old positions keep their takes. Each fully closed old cycle updates the free grid at the current price. New fills keep the ordinary repeat rules.");
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
        try { renderPreview(engine.preview(uiConfig())); }
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
void Window::closeEvent(QCloseEvent* event) {
    if (closeAllowed || engine.orders().empty()) { save(); event->accept(); return; }
    if (!ready || storageFailed) {
        if (QMessageBox::question(this, "Close while disconnected", "Entry cancellation cannot be confirmed. Close and leave exchange orders unchanged?") == QMessageBox::Yes) { save(); event->accept(); }
        else event->ignore(); return;
    }
    closing = true; engine.pause(); save(); log("Closing: waiting for entry cancellations and takes for raced fills"); event->ignore();
    if (engine.blocked || (!engine.settled() && !engine.problem.empty())) {
        closing = false; QMessageBox::warning(this, "Close is blocked", "Resolve the displayed order conflict before closing, or disconnect and manage the orders manually.");
    }
}
