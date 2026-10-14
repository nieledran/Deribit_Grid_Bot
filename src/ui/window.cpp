#include "ui/window.h"
#include "exchange/api_numbers.h"
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QMessageBox>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <stdexcept>

using namespace grid;
using namespace deribit;

Window::Window(const QString& profile, const QString& initialInstrument, QWidget* parent,
    const QUrl& endpoint, bool initialProduction)
    : QMainWindow(parent), client(this), testEndpoint(endpoint)
{
    profilePath = QFileInfo(profile).absoluteFilePath();
    QDir().mkpath(QFileInfo(profile).absolutePath());
    auto profileLock = std::make_unique<QLockFile>(profile + ".lock");
    if (!profileLock->tryLock())
        throw std::runtime_error("This profile is already open in another instance");

    store = std::make_unique<Store>(profile);
    if (!store->load(engine)) {
        engine.profileId = QUuid::createUuid().toString(QUuid::Id128).left(16).toStdString();
        engine.config.instrument = initialInstrument.toStdString();
        engine.config.production = initialProduction;
        const auto error = Engine::validate(engine.config);
        if (!error.empty())
            throw std::runtime_error(error);
    }

    controller = std::make_unique<TradeController>(engine, *store, client);
    profiles = std::make_unique<ProfileController>(
        engine, *store, client, session, std::move(profileLock), this);
    connectProfileController();
    engine.pause();
    engine.disconnected();
    store->save(engine);

    setupUi();
    connect(&client, &Client::notice, this, &Window::log);
    connect(&client, &Client::lost, this, &Window::handleDisconnect);
    connect(&client, &Client::authenticatedSession, this, &Window::initialize);
    connect(&client, &Client::subscription, this, &Window::handleSubscription);

    pumpTimer.setInterval(400);
    connect(&pumpTimer, &QTimer::timeout, this, &Window::pump);
    pumpTimer.start();

    readTimer.setInterval(5000);
    connect(&readTimer, &QTimer::timeout, this, &Window::refreshAccount);
    readTimer.start();

    log("Ready. Set ranges and budgets (0 disables a side), then connect. No market or stop-loss "
        "orders.");
    render();
}

Window::~Window()
{
    client.stop();
}

void Window::handleDisconnect()
{
    marketClock.invalidate();
    ++session;
    ready = false;
    syncing = false;
    busy = false;
    readBusy = false;
    feeRequests.clear();

    engine.disconnected();
    save();

    log("Disconnected: exchange orders remain; waiting for reconciliation");
    render();
}

void Window::handleSubscription(const QString& channel, const QJsonValue& payload)
{
    try {
        if (channel.startsWith("ticker."))
            readMarket(payload.toObject());
        else if (channel.startsWith("user.changes.")) {
            const auto changes = payload.toObject();
            const auto orders = changes["orders"].toArray();
            if (syncing) {
                for (const auto& order : orders)
                    bufferedOrders.append(order);
            } else {
                receiveOrders(orders);
            }

            const auto trades = changes["trades"].toArray();
            if (syncing) {
                for (const auto& trade : trades)
                    bufferedTrades.append(trade);
            } else {
                receiveTrades(trades);
            }

            for (const auto& value : changes["positions"].toArray()) {
                const auto position = value.toObject();
                if (position["instrument_name"].toString() == text(engine.config.instrument))
                    netPosition = accountPosition(position);
            }

            if (!changes["trades"].toArray().isEmpty() && !syncing && !readBusy)
                refreshAccount();
        }
        render();
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
    }
}

void Window::save()
{
    if (storageFailed)
        return;
    try {
        store->save(engine);
    } catch (const std::exception& e) {
        storageFailed = true;
        ready = false;
        engine.pause();
        engine.blocked = true;
        engine.problem = std::string("Storage failure; trading disabled: ") + e.what();
        log(text(engine.problem));
        client.stop();
    }
}

void Window::fail(const QString& message)
{
    engine.blocked = true;
    engine.problem = message.toStdString();
    log(message);
    save();
    render();
}

void Window::connectExchange()
{
    try {
        if (idBox->text().trimmed().isEmpty() || secretBox->text().isEmpty())
            throw std::runtime_error("Enter Client ID and client secret");
        auto next = uiConfig();
        if (!engine.cycles().empty() && (next.instrument != engine.config.instrument ||
                                            next.production != engine.config.production))
            throw std::runtime_error(
                "Use a separate profile for another instrument or environment");
        if (next.production &&
            QMessageBox::question(this, "Production",
                "Connect to Production? Start / Resume can place real orders.") != QMessageBox::Yes)
            return;
        if (engine.cycles().empty())
            engine.config = next;
        save();
        if (storageFailed)
            return;
        ++session;
        ready = false;
        log(next.production ? "Connecting to Production" : "Connecting to Testnet");
        client.connectTo(idBox->text().trimmed(), secretBox->text(), next.production, testEndpoint);
        render();
    } catch (const std::exception& e) {
        QMessageBox::warning(this, "Configuration", QString::fromUtf8(e.what()));
    }
}

void Window::cancelProfileOrders(bool eraseProfile)
{
    const QString message =
        eraseProfile
            ? "Cancel all entries and takes of THIS profile, then delete its saved settings and "
              "history? Other profiles are unchanged. Any exchange position remains open WITHOUT "
              "take protection. A fresh empty profile will be created in the same file."
            : "Cancel all entries and takes of THIS profile despite the reconciliation error? The "
              "exchange position will remain open. Other profiles and manual orders are unchanged.";
    if (QMessageBox::warning(this,
            eraseProfile ? "Delete current profile: " + QFileInfo(profilePath).fileName()
                         : "Cancel bot orders",
            message, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    const bool boundAccount = client.authenticated() && profiles->hasAccountLock();
    const bool canCancel = !busy && !profiles->cancelling() && !storageFailed;
    if (!boundAccount || !canCancel) {
        log("Connect to the bound account before cancelling or deleting this profile");
        return;
    }
    // Invalidate incomplete read callbacks. Persist a cancel-only state before sending
    // any idempotent label cancellation; an interrupted attempt retains the profile.
    ++session;
    ready = false;
    syncing = true;
    busy = true;
    readBusy = false;
    engine.cancelAll();
    save();
    if (storageFailed)
        return;
    render();
    const int epoch = session;
    log("Cancelling exchange orders of this profile by exact labels; saved data retained until "
        "verification");
    profiles->cancelOrders(eraseProfile, epoch);
}

void Window::connectProfileController()
{
    connect(profiles.get(), &ProfileController::positionRead, this, [this](double position) {
        netPosition = position;
    });
    connect(profiles.get(), &ProfileController::failed, this,
        [this](const QString& message, bool cancellation) {
            busy = false;
            if (cancellation)
                syncing = false;
            fail(message);
        });
    connect(profiles.get(), &ProfileController::cancelled, this, [this] {
        finishProfileCancellation();
        log("All exchange orders of this profile cancelled; reconciling saved fills");
        synchronize();
    });
    connect(profiles.get(), &ProfileController::profileRecreated, this,
        &Window::finishProfileRecreation);
    connect(profiles.get(), &ProfileController::historyReadFinished, this, [this] {
        busy = false;
    });
    connect(profiles.get(), &ProfileController::historyCleared, this, &Window::finishHistoryReset);
}

void Window::finishProfileCancellation()
{
    busy = false;
    syncing = false;
}

void Window::finishProfileRecreation()
{
    ++session;
    ++statsRevision;

    absentCreates.clear();
    bufferedOrders = {};
    bufferedTrades = {};
    feeRequests.clear();
    recoveredFeeSkips.clear();

    sessionPnl = 0;
    sessionFees = 0;
    closedTrades = 0;
    feesUnavailable = false;
    statsReady = false;

    finishProfileCancellation();
    setConfig(engine.config);
    log("Current profile cleared and recreated; exchange position remains " +
        QString::number(netPosition, 'f', 8) + " coin. Set ranges and budgets before Start.");
    synchronize();
}

void Window::finishHistoryReset()
{
    save();
    if (!storageFailed) {
        journal->clear();
        log("History and tracked inventory cleared. Settings and exchange position preserved. "
            "Start / Resume to build a fresh grid.");
    }
    render();
}

void Window::startGrid(bool onlyChanged)
{
    try {
        if (!ready || !marketClock.isValid() || marketClock.elapsed() > 10000)
            throw std::runtime_error("Wait for a fresh market quote and successful reconciliation");
        if (engine.cancelEverything && !engine.settled())
            throw std::runtime_error("Wait until cancellation finishes");
        auto next = uiConfig();
        if (next.instrument != engine.config.instrument ||
            next.production != engine.config.production)
            throw std::runtime_error(
                "Reconnect with a separate profile for another instrument/environment");
        previewBox->setChecked(false);
        const auto sides = engine.requestRebuild(next, onlyChanged);
        save();
        log(!sides
                ? "Settings unchanged; current grids retained"
                : QString("Rebuild requested: %1; preserving old takes and reallocating free slots")
                      .arg(sides == 3   ? "Long + Short"
                           : sides == 1 ? "Long"
                                        : "Short"));
        render();
    } catch (const std::exception& e) {
        QMessageBox::warning(this, "Cannot start", QString::fromUtf8(e.what()));
    }
}

void Window::resetHistory()
{
    if (!ready || syncing || busy || !engine.canClearHistory())
        return;
    if (QMessageBox::question(this, "Reset after manual trades",
            "Clear saved cycles/orders, tracked inventory and the log? Settings "
            "and session counters stay. The exchange position stays open and is "
            "not adopted into bot cycles.") != QMessageBox::Yes)
        return;
    busy = true;
    render();
    const int epoch = session;
    profiles->resetHistory(epoch);
}

void Window::submit(const Action& a)
{
    save();
    if (storageFailed)
        return;
    busy = true;
    const QString method = a.kind == Mutation::Create ? (a.buy ? "private/buy" : "private/sell")
                           : a.kind == Mutation::Edit ? "private/edit"
                                                      : "private/cancel";
    const int epoch = session;
    log(method + " " + role(a.role) + " @ " + QString::number(price(a.price), 'f', 4) +
        (a.kind == Mutation::Cancel ? "" : " qty=" + amount(a.total, engine.spec)) + " | " +
        (a.exchangeId.empty() ? text(a.label) : text(a.exchangeId)));
    try {
        controller->execute(a, [this, epoch, a](const RpcResult& r) {
            receiveSubmission(r, epoch, a);
        });
    } catch (...) {
        busy = false;
        throw;
    }
}

void Window::receiveSubmission(const RpcResult& r, int epoch, const grid::Action& a)
{
    if (epoch != session)
        return;
    busy = false;
    try {
        if (!r.ok)
            rejectSubmission(a, r);
        else
            acceptSubmission(r);
        render();
    } catch (const std::exception& e) {
        engine.uncertain(a.label);
        save();
        log(QString::fromUtf8(e.what()));
        ready = false;
        synchronize();
    }
}

void Window::rejectSubmission(const grid::Action& a, const RpcResult& r)
{
    if (r.uncertain) {
        engine.uncertain(a.label);
        save();
        ready = false;
        synchronize();
        return;
    }

    engine.rejected(a.label, r.error.toStdString());
    save();
    log(r.error);
    if (a.kind != Mutation::Create) {
        ready = false;
        synchronize();
    }
}

void Window::acceptSubmission(const RpcResult& r)
{
    const auto root = r.value.toObject();
    const auto order = root.contains("order") ? root["order"].toObject() : root;
    receiveOrder(order, true);
    receiveTrades(root["trades"].toArray());
    save();
    QTimer::singleShot(0, this, &Window::pump);
}

void Window::pump()
{
    const bool canPump = ready && !syncing && !busy && !storageFailed;
    if (!canPump || !client.authenticated()) {
        updateLatency();
        return;
    }
    try {
        if (!marketClock.isValid() || marketClock.elapsed() > 10000) {
            if (engine.running || engine.rebuilding) {
                engine.pause();
                save();
                log("Stale market data: cancelling entries; Resume after a fresh quote");
            }
        }
        auto action = engine.nextAction();
        if (action)
            submit(*action);
        else {
            save();
            if (closing && engine.settled() && !engine.blocked) {
                closeAllowed = true;
                close();
            }
        }
        render();
    } catch (const std::exception& e) {
        fail(QString::fromUtf8(e.what()));
    }
}

void Window::closeEvent(QCloseEvent* event)
{
    if (closeAllowed || engine.orders().empty()) {
        save();
        event->accept();
        return;
    }
    if (!ready || storageFailed) {
        if (QMessageBox::question(this, "Close while disconnected",
                "Entry cancellation cannot be confirmed. Close and leave "
                "exchange orders unchanged?") == QMessageBox::Yes) {
            save();
            event->accept();
        } else
            event->ignore();
        return;
    }
    closing = true;
    engine.pause();
    save();
    log("Closing: waiting for entry cancellations and takes for raced fills");
    event->ignore();
    if (engine.blocked || (!engine.settled() && !engine.problem.empty())) {
        closing = false;
        QMessageBox::warning(this, "Close is blocked",
            "Resolve the displayed order conflict before closing, or disconnect "
            "and manage the orders manually.");
    }
}

void Window::pauseGrid()
{
    engine.pause();
    save();
    log("Pausing: cancelling entries and protecting fills");
    render();
}

void Window::cancelBotOrders()
{
    if (!ready) {
        cancelProfileOrders(false);
        return;
    }
    if (QMessageBox::question(this, "Cancel bot orders",
            "Cancel every entry and take of this profile? The position will remain open.") !=
        QMessageBox::Yes)
        return;
    engine.cancelAll();
    save();
    log("Cancelling all bot orders; position stays open");
    render();
}
