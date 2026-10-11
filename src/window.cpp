#include "window.h"
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
#include <QSplitter>
#include <QTabWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <stdexcept>

using namespace grid;
namespace {
QString text(const std::string& s) { return QString::fromStdString(s); }
double price(Int value) { return static_cast<double>(value) / Scale; }
Int fixed(double value) {
    if (!std::isfinite(value) || value < 0 || value > 1000000000.0) throw std::runtime_error("Invalid API number");
    return static_cast<Int>(std::llround(value * Scale));
}
Int signedFixed(double value) { return value < 0 ? -fixed(-value) : fixed(value); }
double accountPosition(const QJsonObject& position) {
    const auto value = position["size_currency"];
    if (!value.isDouble() || !std::isfinite(value.toDouble())) throw std::runtime_error("Missing account position in base currency");
    return value.toDouble();
}
QString amount(Int lots, const Spec& s) { return QString::number(static_cast<double>(lots) * s.quantityStep / Scale, 'f', 8); }
QString money(Int value) { return QString::number(static_cast<double>(value) / Dollars, 'f', 2); }
Int apiLots(const QJsonValue& value, const Spec& spec) {
    if (!value.isDouble() || spec.quantityStep <= 0) throw std::runtime_error("Missing API quantity/specification");
    const Int units = fixed(value.toDouble()); const Int lots = units / spec.quantityStep;
    if (units % spec.quantityStep) throw std::runtime_error("API quantity is not a multiple of the quantity step");
    return lots;
}
QString role(Role r) { return r == Role::Entry ? "Entry" : "Take"; }
QString state(Status s) {
    switch (s) { case Status::Pending: return "Pending"; case Status::Open: return "Open"; case Status::Uncertain: return "Uncertain";
        case Status::Terminal: return "Closed"; case Status::Rejected: return "Rejected"; } return "Invalid";
}
void cell(QTableWidget* table, int row, int col, const QString& value) { table->setItem(row, col, new QTableWidgetItem(value)); }
void setupTable(QTableWidget* table, const QStringList& headers) {
    table->setColumnCount(headers.size()); table->setHorizontalHeaderLabels(headers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->setAlternatingRowColors(true);
}
}
Window::Window(const QString& profile, const QString& initialInstrument, QWidget* parent, const QUrl& endpoint, bool initialProduction) : QMainWindow(parent), client(this), testEndpoint(endpoint) {
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
    engine.pause(); engine.disconnected();
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
    applyButton = new QPushButton("Apply settings / Rebuild"); cancelButton = new QPushButton("Cancel all bot orders");
    previewBox = new QCheckBox("Preview pending settings");
    buttons->addWidget(startButton); buttons->addWidget(pauseButton); buttons->addWidget(applyButton); buttons->addWidget(cancelButton); buttons->addWidget(previewBox); buttons->addStretch(); layout->addLayout(buttons);
    auto* statusRow = new QHBoxLayout;
    summaryLabel = new QLabel; summaryLabel->setWordWrap(true); statusRow->addWidget(summaryLabel, 1);
    statsLabel = new QLabel; statsLabel->setObjectName("sessionStats");
    statsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statsLabel->setToolTip("P&L is gross, using grid entry/take prices, excluding fees and funding. Fees are actual USDC fees for both entries and takes since the first connection; negative fees are rebates. Incomplete means some fees are unavailable. Each fully executed take order counts once. Counters reset when this window closes.");
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
    connect(clearLogButton, &QPushButton::clicked, journal, &QPlainTextEdit::clear);
    connect(resetButton, &QPushButton::clicked, this, &Window::resetHistory);
    connect(connectButton, &QPushButton::clicked, this, &Window::connectExchange);
    connect(startButton, &QPushButton::clicked, this, &Window::startGrid);
    connect(applyButton, &QPushButton::clicked, this, &Window::startGrid);
    connect(pauseButton, &QPushButton::clicked, this, [this] { engine.pause(); save(); log("Pausing: cancelling entries and protecting fills"); render(); });
    connect(cancelButton, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, "Cancel bot orders", "Cancel every entry and take of this profile? The position will remain open.") != QMessageBox::Yes) return;
        engine.cancelAll(); save(); log("Cancelling all bot orders; position stays open"); render();
    });
    connect(&client, &Client::notice, this, &Window::log);
    connect(&client, &Client::lost, this, [this] {
        ++session; ready = false; syncing = false; busy = false; readBusy = false; feeRequests.clear(); engine.disconnected(); save();
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
                    const auto p = v.toObject(); if (p["instrument_name"].toString() == text(engine.config.instrument)) netPosition = p["size_currency"].toDouble(p["size"].toDouble());
                }
                if (!object["trades"].toArray().isEmpty() && !syncing && !readBusy) refreshAccount();
            }
            render();
        } catch (const std::exception& e) { fail(QString::fromUtf8(e.what())); }
    });
    connect(instrumentBox, &QComboBox::currentTextChanged, this, [this] { render(); });
    pumpTimer.setInterval(400); connect(&pumpTimer, &QTimer::timeout, this, &Window::pump); pumpTimer.start();
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
        if (!engine.cycles.empty() && (next.instrument != engine.config.instrument || next.production != engine.config.production))
            throw std::runtime_error("Use a separate profile for another instrument or environment");
        if (next.production && QMessageBox::question(this, "Production", "Connect to Production? Start / Resume can place real orders.") != QMessageBox::Yes) return;
        if (engine.cycles.empty()) engine.config = next;
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
            if (!engine.cycles.empty() && engine.spec.quantityStep && engine.spec.quantityStep != next.quantityStep) throw std::runtime_error("Quantity step changed; existing state requires manual review");
            for (const auto& v : o["tick_size_steps"].toArray()) { const auto step = v.toObject(); next.tickSteps.emplace_back(fixed(step["above_price"].toDouble()), fixed(step["tick_size"].toDouble())); }
            engine.spec = next; save();
            log(QString("Instrument: tick=%1, quantity step=%2 coin, contract size=%3 coin")
                .arg(o["tick_size"].toDouble(), 0, 'g', 12).arg(o["min_trade_amount"].toDouble(), 0, 'g', 12).arg(o["contract_size"].toDouble(), 0, 'g', 12));
            client.call("private/get_account_summary", {{"currency", "USDC"}, {"extended", true}}, [this, epoch](const RpcResult& a) {
                if (epoch != session) return;
                if (!a.ok) { fail(a.error); return; }
                try {
                    const auto o = a.value.toObject(); const auto id = o["id"];
                    if (id.isUndefined() || id.isNull()) throw std::runtime_error("Account ID is missing from the extended account summary");
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
    syncing = true; ready = false; positionChecks = 0; reconcileLabels.clear(); reconcileIndex = 0;
    for (const auto& o : engine.orders) if (o.live() || o.mutation != Mutation::None) reconcileLabels.push_back(o.label);
    // Entry snapshots are applied before takes so offline entry fills create their inventory first.
    std::stable_sort(reconcileLabels.begin(), reconcileLabels.end(), [this](const auto& a, const auto& b) {
        return engine.order(a)->role == Role::Entry && engine.order(b)->role == Role::Take;
    });
    const int epoch = session;
    client.call("private/get_open_orders_by_instrument", {{"instrument_name", text(engine.config.instrument)}}, [this, epoch](const RpcResult& r) {
        if (epoch != session) return;
        if (!r.ok) { fail(r.error); return; }
        engine.foreign.clear();
        for (const auto& v : r.value.toArray()) {
            const auto o = v.toObject();
            if (!engine.order(o["label"].toString().toStdString())) {
                if (o["label"].toString().startsWith("dg_" + text(engine.profileId) + "_")) { fail("Untracked order with this profile label; manual review required"); return; }
                receiveForeignOrder(o);
            }
        }
        reconcileNext();
    });
}
void Window::reconcileNext() {
    if (reconcileIndex >= reconcileLabels.size()) { completeSync(); return; }
    const auto label = reconcileLabels[reconcileIndex++]; const auto* o = engine.order(label); const int epoch = session;
    const QString method = o->exchangeId.empty() ? "private/get_order_state_by_label" : "private/get_order_state";
    const QJsonObject params = o->exchangeId.empty() ? QJsonObject{{"currency", "USDC"}, {"label", text(label)}} : QJsonObject{{"order_id", text(o->exchangeId)}};
    client.call(method, params, [this, epoch, label](const RpcResult& r) {
        if (epoch != session) return;
        QJsonObject snapshot;
        if (r.value.isArray()) {
            const auto matches = r.value.toArray();
            if (matches.size() == 1) snapshot = matches[0].toObject();
        } else if (r.value.isObject()) snapshot = r.value.toObject();
        if (!r.ok || snapshot["order_id"].toString().isEmpty()) {
            engine.uncertain(label); save(); fail("Cannot resolve saved order " + text(label) + ": " + r.error); syncing = false; return;
        }
        try { receiveOrder(snapshot, true); QTimer::singleShot(100, this, [this, epoch] { if (epoch == session) reconcileNext(); }); }
        catch (const std::exception& ex) { fail(QString::fromUtf8(ex.what())); syncing = false; }
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
                    if (!engine.positionMatches(actual) && positionChecks++ < 2) { QTimer::singleShot(250, this, [this, epoch] { if (epoch == session) completeSync(); }); return; }
                    const bool matched = engine.reconcilePosition(actual);
                    if (!statsReady) feesSince = QDateTime::currentMSecsSinceEpoch();
                    syncing = false; ready = true; statsReady = true; save();
                    log(matched ? "Reconciled own orders and account position. Start / Resume rebuilds free slots." : text(engine.problem));
                    receiveTrades(bufferedTrades); bufferedTrades = {};
                    for (auto it = feeExpected.cbegin(); it != feeExpected.cend(); ++it) refreshFees(it.key());
                    refreshAccount(); render();
                } catch (const std::exception& e) { syncing = false; fail(QString::fromUtf8(e.what())); }
            });
        });
    } catch (const std::exception& e) { fail(QString::fromUtf8(e.what())); }
}
void Window::receiveOrder(const QJsonObject& j, bool resolve) {
    const auto label = j["label"].toString().toStdString(); auto* o = engine.order(label);
    if (!o) {
        receiveForeignOrder(j);
        return;
    }
    if (j["instrument_name"].toString() != text(engine.config.instrument) || j["order_type"].toString() != "limit" ||
        (j["direction"].toString() == "buy") != engine.buys(*o)) throw std::runtime_error("Unexpected instrument/type/direction for a bot order");
    const auto status = j["order_state"].toString();
    if (status != "open" && status != "filled" && status != "cancelled" && status != "rejected") throw std::runtime_error("Unknown exchange order state");
    Update u{label, j["order_id"].toString().toStdString(), apiLots(j["amount"], engine.spec), apiLots(j["filled_amount"], engine.spec),
        j["last_update_timestamp"].toInteger(), fixed(j["price"].toDouble()), status != "open"};
    const bool manualCancel = !resolve && u.terminal && status == "cancelled" && o->mutation == Mutation::None && o->status != Status::Terminal;
    const Int previousFilled = o->filled;
    const bool previouslyClosed = o->status == Status::Terminal && o->filled == o->total;
    engine.apply(u); if (resolve) engine.resolved(label);
    if (o->filled > previousFilled) ++positionRevision;
    if (statsReady && o->filled > previousFilled) {
        const QString id = text(o->exchangeId);
        feeExpected[id] += o->filled - previousFilled;
        // Give the subscription/submit response a chance to supply fees before an extra read.
        QTimer::singleShot(250, this, [this, id] { refreshFees(id); });
    }
    if (statsReady && o->role == Role::Take) {
        const auto* c = engine.cycle(o->cycle);
        const Int delta = o->filled - previousFilled;
        const Int spread = c->side == Side::Long ? c->take - c->entry : c->entry - c->take;
        sessionPnl += price(spread) * static_cast<double>(delta) * engine.spec.quantityStep / Scale;
        if (!previouslyClosed && o->status == Status::Terminal && o->filled == o->total) ++closedTrades;
    }
    if (manualCancel) { engine.blocked = true; engine.problem = "A bot order was cancelled externally; review and Resume explicitly"; }
    save();
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
    std::vector<QJsonObject> sorted; for (const auto& v : events) sorted.push_back(v.toObject());
    std::stable_sort(sorted.begin(), sorted.end(), [this](const auto& a, const auto& b) {
        const auto* oa = engine.order(a["label"].toString().toStdString()); const auto* ob = engine.order(b["label"].toString().toStdString());
        const int ra = !oa ? 1 : oa->role == Role::Entry ? 0 : 2;
        const int rb = !ob ? 1 : ob->role == Role::Entry ? 0 : 2;
        return ra < rb;
    });
    for (const auto& o : sorted) receiveOrder(o);
}
void Window::receiveTrades(const QJsonArray& trades) {
    if (!statsReady) return;
    for (const auto& value : trades) {
        const auto t = value.toObject(); const QString id = t["order_id"].toString();
        if (id.isEmpty() || t["instrument_name"].toString() != text(engine.config.instrument) ||
            !t["timestamp"].isDouble() || t["timestamp"].toInteger() < feesSince) continue;
        const auto* labelled = engine.order(t["label"].toString().toStdString());
        const bool own = (labelled && (labelled->exchangeId.empty() || text(labelled->exchangeId) == id)) ||
            std::any_of(engine.orders.begin(), engine.orders.end(), [&](const Order& o) { return text(o.exchangeId) == id; });
        const QString tradeId = t["trade_id"].toString(); const QString key = id + "/" + tradeId;
        if (!own || tradeId.isEmpty() || feeTrades.contains(key) || !t["fee"].isDouble() ||
            t["fee_currency"].toString() != "USDC" || !std::isfinite(t["fee"].toDouble())) continue;
        // Fee data must never change inventory or prevent trading when unavailable/malformed.
        try {
            const Int lots = apiLots(t["amount"], engine.spec); if (lots <= 0) continue;
            feeTrades.insert(key); feeCovered[id] += lots; sessionFees += t["fee"].toDouble();
        } catch (const std::exception&) { /* Leave the fee counter incomplete. */ }
    }
}
void Window::refreshFees(const QString& id) {
    if (!ready || syncing || !client.authenticated() || feeRequests.contains(id) || feeCovered.value(id) >= feeExpected.value(id)) return;
    feeRequests.insert(id); const int epoch = session; const Int expected = feeExpected.value(id);
    client.call("private/get_user_trades_by_order", {{"order_id", id}, {"sorting", "asc"}}, [this, id, epoch, expected](const RpcResult& r) {
        if (epoch != session) return;
        feeRequests.remove(id);
        if (r.ok && r.value.isArray()) receiveTrades(r.value.toArray());
        else log("Fees unavailable for " + id + "; gross P&L remains available");
        render();
        // A fill received during this read needs a newer snapshot, but missing fees alone do not cause retries.
        if (feeExpected.value(id) > expected) refreshFees(id);
    });
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
            if (r.ok) {
                netPosition = accountPosition(r.value.toObject());
                if (ready && !syncing && !busy && !engine.positionBlocked && revision == positionRevision && !engine.positionMatches(signedFixed(netPosition))) {
                    readBusy = false; log("Account position changed outside tracked fills; reconciling before further requests"); synchronize(); return;
                }
            } else log(r.error);
        } catch (const std::exception& e) { readBusy = false; log(QString::fromUtf8(e.what())); return; }
        client.call("public/ticker", {{"instrument_name", text(engine.config.instrument)}}, [this, epoch](const RpcResult& t) {
            if (epoch != session) return; readBusy = false;
            try { if (t.ok) readMarket(t.value.toObject()); else log(t.error); render(); }
            catch (const std::exception& e) { fail(QString::fromUtf8(e.what())); }
        });
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
void Window::submit(const Action& a) {
    const auto* owned = engine.order(a.label);
    if (!owned || !a.label.starts_with("dg_" + engine.profileId + "_") || owned->exchangeId != a.exchangeId ||
        owned->role != a.role || owned->mutation != a.kind ||
        std::any_of(engine.foreign.begin(), engine.foreign.end(), [&](const ForeignOrder& f) { return f.id == a.exchangeId && !a.exchangeId.empty(); })) {
        fail("Trading request refused: order ownership is not verified"); return;
    }
    save(); if (storageFailed) return; busy = true;
    QString method; QJsonObject params;
    const double quantity = static_cast<double>(a.total) * engine.spec.quantityStep / Scale;
    if (a.kind == Mutation::Create) {
        method = a.buy ? "private/buy" : "private/sell";
        params = {{"instrument_name", text(engine.config.instrument)}, {"amount", quantity}, {"type", "limit"}, {"price", price(a.price)},
            {"time_in_force", "good_til_cancelled"}, {"post_only", false}, {"reduce_only", false}, {"label", text(a.label)}};
    } else if (a.kind == Mutation::Edit) {
        method = "private/edit"; params = {{"order_id", text(a.exchangeId)}, {"amount", quantity}, {"price", price(a.price)}, {"post_only", false}};
    } else { method = "private/cancel"; params = {{"order_id", text(a.exchangeId)}}; }
    const int epoch = session;
    log(method + " " + role(a.role) + " @ " + QString::number(price(a.price), 'f', 4) + (a.kind == Mutation::Cancel ? "" : " qty=" + amount(a.total, engine.spec)) + " | " + (a.exchangeId.empty() ? text(a.label) : text(a.exchangeId)));
    client.call(method, params, [this, epoch, a](const RpcResult& r) {
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
            }
            render();
        } catch (const std::exception& e) { engine.uncertain(a.label); fail(QString::fromUtf8(e.what())); }
    }, true);
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
void Window::render() {
    if (!summaryLabel) return;
    // Instrument and environment belong to the selected profile and cannot change in place.
    instrumentBox->setEnabled(false); environmentBox->setEnabled(false);
    connectButton->setEnabled(!busy && !storageFailed && !closing); connectButton->setText(client.authenticated() ? "Reconnect / Reconcile" : "Connect");
    startButton->setEnabled(ready && !syncing && !busy && !engine.positionBlocked && !storageFailed && !closing && marketClock.isValid() && marketClock.elapsed() <= 10000);
    resetButton->setEnabled(ready && !syncing && !busy && !closing && !storageFailed && engine.canClearHistory());
    applyButton->setEnabled(startButton->isEnabled()); pauseButton->setEnabled(ready); cancelButton->setEnabled(ready);
    idBox->setEnabled(!client.authenticated()); secretBox->setEnabled(!client.authenticated());
    connectionLabel->setText(syncing ? "Reconciling" : ready ? "Connected" : "Disconnected");
    environmentBox->setStyleSheet(environmentBox->currentIndex() ? "color:#b91c1c;font-weight:bold" : "");
    QString mode = engine.positionBlocked ? "Position mismatch" : engine.cancelEverything ? "Cancelling all" : engine.blocked ? "Blocked" : engine.rebuilding ? "Rebuilding" : engine.running ? "Running" : "Paused";
    bool feesComplete = true;
    for (auto it = feeExpected.cbegin(); it != feeExpected.cend(); ++it) if (feeCovered.value(it.key()) < it.value()) feesComplete = false;
    statsLabel->setText(QString("Approx. P&L: %1 USDC\nFees (entry + take): %2 USDC%3\nClosed trades: %4")
        .arg(sessionPnl, 0, 'f', 4).arg(sessionFees, 0, 'f', 4).arg(feesComplete ? "" : " (incomplete)").arg(closedTrades));
    const QString grids = engine.config.longs.budget && engine.config.shorts.budget ? "Long + Short" : engine.config.longs.budget ? "Long" : engine.config.shorts.budget ? "Short" : "Disabled";
    summaryLabel->setText(QString("%1 | Grids: %2 | Mid: %3 | Net position: %4 coin\nLong reserved: %5 / %6 USDC (%7 / %8 slots) | Short reserved: %9 / %10 USDC (%11 / %12 slots)%13")
        .arg(mode, grids, QString::number(price(engine.market), 'f', 4), QString::number(netPosition, 'f', 8))
        .arg(money(engine.used(Side::Long)), money(engine.config.longs.budget)).arg(engine.occupiedSlots(Side::Long)).arg(engine.config.longs.levels)
        .arg(money(engine.used(Side::Short)), money(engine.config.shorts.budget)).arg(engine.occupiedSlots(Side::Short)).arg(engine.config.shorts.levels)
        .arg(engine.problem.empty() ? "" : "\n" + text(engine.problem)));
    cyclesTable->setRowCount(0);
    for (const auto& c : engine.cycles) {
        if (c.retired && c.held == 0 && std::none_of(engine.orders.begin(), engine.orders.end(), [&](const Order& o) { return o.cycle == c.id && o.live(); })) continue;
        const int row = cyclesTable->rowCount(); cyclesTable->insertRow(row);
        const QStringList values{QString::number(c.id), text(sideName(c.side)), QString::number(c.generation), QString::number(price(c.entry), 'f', 4),
            QString::number(price(c.take), 'f', 4), amount(c.capacity, engine.spec), amount(c.held, engine.spec), money(engine.nominal(c.retired ? c.held : c.capacity, c.entry)), c.retired ? "Old: close only" : "Current: repeat"};
        for (int col = 0; col < values.size(); ++col) cell(cyclesTable, row, col, values[col]);
    }
    ordersTable->setRowCount(0);
    for (const auto& o : engine.orders) {
        if (!o.live() && o.mutation == Mutation::None && o.status != Status::Rejected) continue;
        const int row = ordersTable->rowCount(); ordersTable->insertRow(row);
        const QStringList values{QString::number(o.cycle), role(o.role) + (engine.buys(o) ? " / Buy" : " / Sell"), QString::number(price(o.price), 'f', 4),
            amount(o.total, engine.spec), amount(o.filled, engine.spec), state(o.status), text(o.exchangeId)};
        for (int col = 0; col < values.size(); ++col) cell(ordersTable, row, col, values[col]);
    }
    for (const auto& f : engine.foreign) {
        const int row = ordersTable->rowCount(); ordersTable->insertRow(row);
        cell(ordersTable, row, 0, "Other / Manual"); cell(ordersTable, row, 1, (f.buy ? QString("Buy") : QString("Sell")) + (f.conditional ? " / Trigger" : "")); cell(ordersTable, row, 2, f.price > 0 ? QString::number(price(f.price), 'f', 4) : "Market on trigger");
        cell(ordersTable, row, 5, "Untouched"); cell(ordersTable, row, 6, text(f.id));
    }
    if (previewBox->isChecked()) {
        try { renderPreview(engine.preview(uiConfig())); }
        catch (const std::exception& e) {
            previewTable->setRowCount(1); for (int col = 0; col < previewTable->columnCount(); ++col) cell(previewTable, 0, col, "");
            cell(previewTable, 0, 5, QString::fromUtf8(e.what()));
        }
    } else renderPreview(engine);
}
void Window::renderPreview(const Engine& view) {
    previewTable->setRowCount(static_cast<int>(view.lattice.size()));
    int index = 0;
    for (const auto& l : view.lattice) {
        const Cycle* existing = nullptr;
        for (const auto& c : view.cycles) if (!c.retired && c.entry == l.entry && c.side == l.side) existing = &c;
        const auto& b = l.side == Side::Long ? view.config.longs : view.config.shorts;
        const int free = b.levels - view.occupiedSlots(l.side);
        const Int q = existing ? existing->capacity : free > 0 ? view.lotsFor(std::max<Int>(0, b.budget - view.used(l.side)) / free, l.entry) : 0;
        cell(previewTable, index, 0, text(sideName(l.side)) + " / " + QString::number(index + 1)); cell(previewTable, index, 1, QString::number(price(l.entry), 'f', 4));
        cell(previewTable, index, 2, QString::number(price(l.take), 'f', 4)); cell(previewTable, index, 3, amount(q, view.spec));
        cell(previewTable, index, 4, money(view.nominal(q, l.entry))); cell(previewTable, index, 5, existing ? "Allocated" : free > 0 ? "Waiting / Available" : "Waiting for a slot"); ++index;
    }
}
void Window::closeEvent(QCloseEvent* event) {
    if (closeAllowed || engine.orders.empty()) { save(); event->accept(); return; }
    if (!ready || storageFailed) {
        if (QMessageBox::question(this, "Close while disconnected", "Entry cancellation cannot be confirmed. Close and leave exchange orders unchanged?") == QMessageBox::Yes) { save(); event->accept(); }
        else event->ignore(); return;
    }
    closing = true; engine.pause(); save(); log("Closing: waiting for entry cancellations and takes for raced fills"); event->ignore();
    if (engine.blocked || (!engine.settled() && !engine.problem.empty())) {
        closing = false; QMessageBox::warning(this, "Close is blocked", "Resolve the displayed order conflict before closing, or disconnect and manage the orders manually.");
    }
}
